#include "player.hpp"
#include "url_utils.hpp"
#include "diagnostics.hpp"
#include "quickjs.h"
#include <chrono>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <utility>
#include <vector>

extern "C" {
extern const unsigned char cryget_acorn_source[];
extern const unsigned long cryget_acorn_size;
}

namespace cryget {
namespace {
// C++ adaptation of the player statement filtering / alr entry-point approach
// documented by yt-dlp/ejs (Unlicense). No ejs or yt-dlp runtime is used.
struct Value {
    JSContext* ctx;
    JSValue value;
    Value(JSContext* c, JSValue v) : ctx(c), value(v) {}
    ~Value() { JS_FreeValue(ctx, value); }
    Value(const Value&) = delete;
    Value& operator=(const Value&) = delete;
    Value(Value&& other) noexcept : ctx(other.ctx), value(other.value) { other.value = JS_UNDEFINED; }
    Value& operator=(Value&& other) noexcept {
        if(this!=&other){JS_FreeValue(ctx,value);ctx=other.ctx;value=other.value;other.value=JS_UNDEFINED;}
        return *this;
    }
    Value get(const char* key) const { return {ctx, JS_GetPropertyStr(ctx, value, key)}; }
    Value at(uint32_t index) const { return {ctx, JS_GetPropertyUint32(ctx, value, index)}; }
    uint32_t number() const { uint32_t out=0; JS_ToUint32(ctx,&out,value); return out; }
    uint32_t size() const { return get("length").number(); }
    std::string text() const {
        if(JS_IsNull(value)||JS_IsUndefined(value)) return {};
        size_t length=0;
        const char* s=JS_ToCStringLen(ctx,&length,value);
        if(!s) return {};
        std::string out(s,length);JS_FreeCString(ctx,s);return out;
    }
    std::string type() const { return get("type").text(); }
};

struct Engine {
    JSRuntime* runtime=nullptr;
    JSContext* context=nullptr;
    const std::atomic<bool>* canceled;
    std::chrono::steady_clock::time_point deadline;
    explicit Engine(const std::atomic<bool>* stop, size_t memory=128*1024*1024) : canceled(stop) {
        runtime=JS_NewRuntime();
        if(!runtime) throw std::runtime_error("Cannot allocate JavaScript runtime");
        JS_SetMemoryLimit(runtime,memory);
        JS_SetMaxStackSize(runtime,512*1024);
        reset(15);
        JS_SetInterruptHandler(runtime,[](JSRuntime*,void* opaque) -> int {
            auto& e=*static_cast<Engine*>(opaque);
            return (e.canceled&&e.canceled->load())||std::chrono::steady_clock::now()>e.deadline;
        },this);
        context=JS_NewContext(runtime);
        if(!context) {JS_FreeRuntime(runtime);runtime=nullptr;throw std::runtime_error("Cannot create JavaScript context");}
    }
    ~Engine(){JS_FreeContext(context);JS_FreeRuntime(runtime);}
    void reset(int seconds=3){deadline=std::chrono::steady_clock::now()+std::chrono::seconds(seconds);}
    Value checked(JSValue v,const char* stage) {
        if(JS_IsException(v)) {
            Value error(context,JS_GetException(context));
            if(canceled&&canceled->load()) throw std::runtime_error("Download canceled");
            if(std::chrono::steady_clock::now()>deadline) throw std::runtime_error("Player JavaScript exceeded its time limit");
            // Do not expose arbitrary script exception strings (which may contain media URLs).
            auto kind=error.get("name").text();
            throw std::runtime_error(std::string(stage)+" ("+(kind.empty()?"JavaScript error":kind)+")");
        }
        return {context,v};
    }
    Value eval(const std::string& code,const char* stage) {
        return checked(JS_Eval(context,code.c_str(),code.size(),stage,JS_EVAL_TYPE_GLOBAL),stage);
    }
    Value string(const std::string& s){return {context,JS_NewStringLen(context,s.data(),s.size())};}
    Value global(){return {context,JS_GetGlobalObject(context)};}
    Value call(const Value& fn,const Value& self,std::vector<JSValue> args,const char* stage) {
        return checked(JS_Call(context,fn.value,self.value,static_cast<int>(args.size()),args.data()),stage);
    }
};

// Acorn's offsets are UTF-16 positions; source strings in C++ are UTF-8.
std::vector<size_t> byte_offsets(const std::string& source) {
    std::vector<size_t> out;
    out.reserve(source.size()+1);
    for(size_t i=0;i<source.size();) {
        const unsigned char c=source[i];
        size_t n=c<128?1:c<224?2:c<240?3:4;
        out.push_back(i);if(n==4)out.push_back(i);
        i+=n;
    }
    out.push_back(source.size());return out;
}

bool is_entry(const Value& function) {
    if(function.type()!="FunctionExpression"&&function.type()!="FunctionDeclaration")return false;
    auto statements=function.get("body").get("body");
    for(uint32_t i=0;i<statements.size();++i) {
        auto s=statements.at(i);
        if(s.type()!="ExpressionStatement")continue;
        auto call=s.get("expression");if(call.type()!="CallExpression")continue;
        auto callee=call.get("callee");
        if(callee.type()!="MemberExpression"||callee.get("object").type()!="Identifier")continue;
        auto args=call.get("arguments");
        if(args.size()==2&&args.at(0).get("value").text()=="alr"&&args.at(1).get("value").text()=="yes")return true;
    }
    return false;
}

const char* environment=R"JS(
var window=globalThis, self=globalThis;
var document=Object.create(null), navigator=Object.create(null);
var XMLHttpRequest={prototype:{}};
var location={hash:'',host:'www.youtube.com',hostname:'www.youtube.com',
  href:'https://www.youtube.com/watch?v=aaaaaaaaaaa',origin:'https://www.youtube.com',
  pathname:'/watch',port:'',protocol:'https:',search:'?v=aaaaaaaaaaa',username:'',password:''};
)JS";
} // namespace

std::string PlayerSolver::prepare(const std::string& source,const std::atomic<bool>* canceled) {
    if(source.size()>8*1024*1024)throw std::runtime_error("Player script is too large");
    Engine engine(canceled,384*1024*1024);
    JS_SetGCThreshold(engine.runtime,192*1024*1024);
    engine.reset(60);
    engine.eval(std::string(reinterpret_cast<const char*>(cryget_acorn_source),cryget_acorn_size),"JavaScript parser initialization failed");
    // Retain source ranges and possible entry calls, not the hundreds of thousands
    // of AST nodes inside unrelated function bodies. Actual player code is untouched.
    engine.eval(R"JS(
var __crygetParser=acorn.Parser.extend(Base=>class extends Base {
  compact(node) {
    if ((node.type==='FunctionExpression'||node.type==='FunctionDeclaration') && node.start>256) {
      node.body.body=node.body.body.filter(s=>s.type==='ExpressionStatement' &&
        s.expression.type==='CallExpression' && s.expression.arguments.length===2 &&
        s.expression.arguments[0].type==='Literal' && s.expression.arguments[0].value==='alr');
    }
    return node;
  }
  finishNode(node,type) { return this.compact(super.finishNode(node,type)); }
  finishNodeAt(node,type,pos,loc) { return this.compact(super.finishNodeAt(node,type,pos,loc)); }
});
)JS","JavaScript parser setup failed");
    auto global=engine.global();auto acorn=global.get("__crygetParser");auto parse=acorn.get("parse");
    auto input=engine.string(source);
    Value options(engine.context,JS_NewObject(engine.context));
    JS_SetPropertyStr(engine.context,options.value,"ecmaVersion",JS_NewInt32(engine.context,2026));
    log_event("player.parse.start","bytes="+std::to_string(source.size()));
    auto ast=engine.call(parse,acorn,{input.value,options.value},"Cannot parse player JavaScript");
    log_event("player.parse.ready","AST available");
    auto statements=ast.get("body");
    if(statements.size()<1||statements.size()>2)throw std::runtime_error("Unrecognized player script wrapper");
    auto statement=statements.at(statements.size()-1);
    auto expression=statement.get("expression");auto callee=expression.get("callee");
    if(callee.type()=="MemberExpression")callee=callee.get("object");
    if(callee.type()!="FunctionExpression")throw std::runtime_error("Unrecognized player function wrapper");
    auto block=callee.get("body");auto body=block.get("body");
    auto offsets=byte_offsets(source);
    auto pos=[&](const Value& node,const char* key){const auto i=node.get(key).number();if(i>=offsets.size())throw std::runtime_error("Invalid player source position");return offsets[i];};
    auto code=[&](const Value& node){auto a=pos(node,"start"),b=pos(node,"end");if(b<a)throw std::runtime_error("Invalid player source range");return source.substr(a,b-a);};
    std::vector<std::string> entries;
    std::string prepared=environment;
    prepared+=source.substr(0,pos(block,"start")+1);
    for(uint32_t i=0;i<body.size();++i) {
        if(canceled&&canceled->load())throw std::runtime_error("Download canceled");
        auto s=body.at(i);const auto type=s.type();
        if(type=="ExpressionStatement") {
            auto expr=s.get("expression");const auto et=expr.type();
            if(et!="AssignmentExpression"&&et!="Literal")continue;
            if(et=="AssignmentExpression"&&is_entry(expr.get("right")))entries.push_back(code(expr.get("left")));
        } else if(type=="VariableDeclaration") {
            auto declarations=s.get("declarations");
            if(i==0&&declarations.size()==1&&declarations.at(0).get("id").get("name").text()=="window") {
                prepared+="var window=globalThis;\n";continue;
            }
            for(uint32_t d=0;d<declarations.size();++d) {
                auto decl=declarations.at(d);
                if(is_entry(decl.get("init")))entries.push_back(code(decl.get("id")));
            }
        } else if(type=="FunctionDeclaration"&&is_entry(s))entries.push_back(code(s.get("id")));
        prepared+=code(s)+"\n";
    }
    if(entries.empty())throw std::runtime_error("Player signature function was not found; this player version is not supported");
    prepared+=";globalThis.__crygetSolvers=[";
    for(size_t i=0;i<entries.size();++i){if(i)prepared+=",";prepared+=entries[i];}
    prepared+="];\n"+source.substr(pos(block,"end")-1);
    log_event("player.prepared","entries="+std::to_string(entries.size())+" bytes="+std::to_string(prepared.size()));
    return prepared;
}

struct PlayerSolver::Impl {
    Engine engine;
    std::map<std::pair<std::string,std::string>,PlayerResult> cache;
    Impl(const std::string& prepared,const std::atomic<bool>* canceled):engine(canceled) {
        engine.eval(prepared,"Player initialization failed");
    }
};
PlayerSolver::PlayerSolver(const std::string& prepared,const std::atomic<bool>* canceled):impl(std::make_unique<Impl>(prepared,canceled)){}
PlayerSolver::~PlayerSolver()=default;

PlayerResult PlayerSolver::solve(const std::string& signature,const std::string& n) {
    auto key=std::make_pair(signature,n);const auto found=impl->cache.find(key);
    if(found!=impl->cache.end())return found->second;
    auto& e=impl->engine;e.reset();auto global=e.global();auto solvers=global.get("__crygetSolvers");
    PlayerResult result;bool obtained=false;
    for(uint32_t i=0;i<solvers.size();++i) {
        try {
            auto fn=solvers.at(i);auto dummy=e.string("https://www.youtube.com/watch?v=aaaaaaaaaaa");
            auto param=e.string("s");auto sig=e.string(url_encode(signature));
            auto url=e.call(fn,global,{dummy.value,param.value,signature.empty()?JS_UNDEFINED:sig.value},"Player signature transformation failed");
            if(!n.empty()) {
                auto set=url.get("set");auto nk=e.string("n");auto nv=e.string(n);
                e.call(set,url,{nk.value,nv.value},"Player n parameter setup failed");
                Value proto(e.context,JS_GetPrototype(e.context,url.value));
                JSPropertyEnum* properties=nullptr;uint32_t count=0;
                if(JS_GetOwnPropertyNames(e.context,&properties,&count,proto.value,JS_GPN_STRING_MASK)<0)
                    throw std::runtime_error("Cannot read player URL methods");
                std::string transform;
                for(uint32_t k=0;k<count;++k){
                    const char* text=JS_AtomToCString(e.context,properties[k].atom);
                    std::string name=text?text:"";if(text)JS_FreeCString(e.context,text);
                    if(name!="constructor"&&name!="set"&&name!="get"&&name!="clone"&&!name.empty()){transform=name;break;}
                }
                JS_FreePropertyEnum(e.context,properties,count);
                if(transform.empty())throw std::runtime_error("Player n transformation method is missing");
                auto method=url.get(transform.c_str());e.call(method,url,{},"Player n transformation failed");
            }
            auto get=url.get("get");auto sk=e.string("s");auto nk=e.string("n");
            PlayerResult current;
            if(!signature.empty())current.signature=url_decode(e.call(get,url,{sk.value},"Cannot read player signature").text());
            if(!n.empty())current.n=e.call(get,url,{nk.value},"Cannot read player n parameter").text();
            if((!signature.empty()&&current.signature.empty())||(!n.empty()&&(current.n.empty()||current.n.find("enhanced_except_")==0)))
                throw std::runtime_error("Player returned an invalid signature");
            if(obtained&&(result.signature!=current.signature||result.n!=current.n))throw std::logic_error("Player signature functions returned conflicting results");
            result=current;obtained=true;
        } catch(const std::runtime_error&) {
            if(solvers.size()==1)throw;
        }
    }
    if(!obtained)throw std::runtime_error("All player signature functions failed");
    impl->cache.emplace(std::move(key),result);return result;
}

std::string resolve_media_url(const std::string& direct,const std::string& cipher,PlayerSolver* solver) {
    std::string url=direct.empty()?query_value(cipher,"url"):direct;
    if(url.rfind("https://",0)!=0)throw std::runtime_error("The media address is missing or is not HTTPS");
    const auto signature=query_value(cipher,"s");const auto n=url_parameter(url,"n");
    if(!signature.empty()||!n.empty()) {
        if(!solver)throw std::runtime_error("This video needs a player signature transformation");
        auto result=solver->solve(signature,n);
        if(!signature.empty()) {auto name=query_value(cipher,"sp");url=set_url_parameter(url,name.empty()?"signature":name,result.signature);}
        if(!n.empty())url=set_url_parameter(url,"n",result.n);
    }
    return url;
}
} // namespace cryget
