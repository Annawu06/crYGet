#include "player.hpp"
#include "url_utils.hpp"
#include "diagnostics.hpp"
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>
#include <node.h>
#include <uv.h>
#include <v8.h>

extern "C" {
extern const unsigned char cryget_acorn_source[];
extern const unsigned long cryget_acorn_size;
}

namespace cryget {
namespace {
std::once_flag v8_once;
struct NodeRuntime {
    decltype(node::InitializeOncePerProcess(std::vector<std::string>{})) initialized;
    ~NodeRuntime(){if(initialized)node::TearDownOncePerProcess();}
} node_runtime;

void initialize_v8() {
    std::call_once(v8_once,[]{
        node_runtime.initialized=node::InitializeOncePerProcess({"crYGet"});
        if(!node_runtime.initialized||node_runtime.initialized->early_return())
            throw std::runtime_error("Cannot initialize embedded V8 runtime");
    });
}

struct Engine;
struct Value {
    Engine* engine=nullptr;
    v8::Global<v8::Value> handle;
    Value(Engine* e,v8::Local<v8::Value> value);
    ~Value(){handle.Reset();}
    Value(const Value&)=delete;
    Value& operator=(const Value&)=delete;
    Value(Value&& other) noexcept:engine(other.engine),handle(std::move(other.handle)){other.engine=nullptr;}
    Value& operator=(Value&& other) noexcept {
        if(this!=&other){handle.Reset();engine=other.engine;handle=std::move(other.handle);other.engine=nullptr;}
        return *this;
    }
    v8::Local<v8::Value> local() const;
    Value get(const char* key) const;
    Value at(uint32_t index) const;
    void set_number(const char* key,int value) const;
    uint32_t number() const;
    uint32_t size() const;
    std::string text() const;
    std::string type() const;
};

struct Engine {
    v8::Isolate* isolate=nullptr;
    v8::Global<v8::Context> context;
    node::ArrayBufferAllocator* allocator=nullptr;
    const std::atomic<bool>* canceled=nullptr;
    std::chrono::steady_clock::time_point deadline;
    std::mutex timer_mutex;
    std::condition_variable timer_changed;
    std::thread timer;
    bool active=false,stopping=false,timed_out=false;

    explicit Engine(const std::atomic<bool>* stop,size_t memory=128*1024*1024):canceled(stop) {
        initialize_v8();
        allocator=node::CreateArrayBufferAllocator();
        if(!allocator)throw std::runtime_error("Cannot allocate JavaScript runtime memory");
        isolate=v8::Isolate::Allocate();
        if(!isolate){node::FreeArrayBufferAllocator(allocator);allocator=nullptr;throw std::runtime_error("Cannot allocate JavaScript runtime");}
        auto* platform=node_runtime.initialized->platform();
        platform->RegisterIsolate(isolate,uv_default_loop());
        v8::Isolate::CreateParams params;params.array_buffer_allocator=allocator;
        params.constraints.set_max_old_generation_size_in_bytes(memory);
        v8::Isolate::Initialize(isolate,params);
        node::SetIsolateUpForNode(isolate);
        {
            v8::Isolate::Scope isolate_scope(isolate);v8::HandleScope handles(isolate);
            context.Reset(isolate,v8::Context::New(isolate));
        }
        reset(15);
        timer=std::thread([this]{watchdog();});
    }
    ~Engine(){
        {std::lock_guard lock(timer_mutex);stopping=true;timer_changed.notify_all();}
        if(timer.joinable())timer.join();
        context.Reset();
        if(isolate) {
            isolate->Dispose();
            node_runtime.initialized->platform()->UnregisterIsolate(isolate);
        }
        if(allocator)node::FreeArrayBufferAllocator(allocator);
    }
    void reset(int seconds=3){std::lock_guard lock(timer_mutex);deadline=std::chrono::steady_clock::now()+std::chrono::seconds(seconds);}
    void watchdog(){
        std::unique_lock lock(timer_mutex);
        while(!stopping) {
            timer_changed.wait_for(lock,std::chrono::milliseconds(25));
            if(stopping)break;
            if(!active)continue;
            const bool canceled_now=canceled&&canceled->load();
            const bool expired=std::chrono::steady_clock::now()>deadline;
            if(canceled_now||expired) {
                timed_out=expired&&!canceled_now;
                auto* target=isolate;lock.unlock();target->TerminateExecution();lock.lock();
            }
        }
    }
    void begin(){std::lock_guard lock(timer_mutex);timed_out=false;active=true;timer_changed.notify_all();}
    bool end(){std::lock_guard lock(timer_mutex);active=false;timer_changed.notify_all();return timed_out;}
    v8::Local<v8::Context> local_context() const{return context.Get(isolate);}
    [[noreturn]] void throw_failure(const char* stage,bool timed,const v8::TryCatch& catcher) {
        const bool was_canceled=canceled&&canceled->load();
        if(isolate->IsExecutionTerminating())isolate->CancelTerminateExecution();
        if(was_canceled)throw std::runtime_error("Download canceled");
        if(timed||catcher.HasTerminated())throw std::runtime_error("Player JavaScript exceeded its time limit");
        v8::Local<v8::Value> exception=catcher.Exception();
        std::string kind="JavaScript error";
        if(!exception.IsEmpty()&&exception->IsObject()) {
            v8::Local<v8::Value> name;
            if(exception.As<v8::Object>()->Get(local_context(),v8::String::NewFromUtf8Literal(isolate,"name")).ToLocal(&name)&&name->IsString()) {
                v8::String::Utf8Value text(isolate,name);if(*text)kind=*text;
            }
        }
        throw std::runtime_error(std::string(stage)+" ("+kind+")");
    }
    Value eval(const std::string& code,const char* stage) {
        v8::Isolate::Scope isolate_scope(isolate);v8::HandleScope handles(isolate);
        auto ctx=local_context();v8::Context::Scope context_scope(ctx);v8::TryCatch catcher(isolate);
        begin();
        v8::Local<v8::String> source;
        const bool made=v8::String::NewFromUtf8(isolate,code.data(),v8::NewStringType::kNormal,static_cast<int>(code.size())).ToLocal(&source);
        v8::Local<v8::Script> script;v8::Local<v8::Value> result;
        bool ok=made&&v8::Script::Compile(ctx,source).ToLocal(&script)&&script->Run(ctx).ToLocal(&result);
        const bool timed=end();
        if(!ok)throw_failure(stage,timed,catcher);
        if(timed)throw std::runtime_error("Player JavaScript exceeded its time limit");
        return Value(this,result);
    }
    Value string(const std::string& text) {
        v8::Isolate::Scope isolate_scope(isolate);v8::HandleScope handles(isolate);
        v8::Local<v8::String> value;
        if(!v8::String::NewFromUtf8(isolate,text.data(),v8::NewStringType::kNormal,static_cast<int>(text.size())).ToLocal(&value))
            throw std::runtime_error("Cannot allocate JavaScript string");
        return Value(this,value);
    }
    Value undefined(){v8::Isolate::Scope isolate_scope(isolate);v8::HandleScope handles(isolate);return Value(this,v8::Undefined(isolate));}
    Value object(){v8::Isolate::Scope isolate_scope(isolate);v8::HandleScope handles(isolate);v8::Context::Scope context_scope(local_context());return Value(this,v8::Object::New(isolate));}
    Value global(){v8::Isolate::Scope isolate_scope(isolate);v8::HandleScope handles(isolate);return Value(this,local_context()->Global());}
    Value call(const Value& fn,const Value& self,std::initializer_list<const Value*> args,const char* stage) {
        v8::Isolate::Scope isolate_scope(isolate);v8::HandleScope handles(isolate);
        auto ctx=local_context();v8::Context::Scope context_scope(ctx);v8::TryCatch catcher(isolate);
        if(!fn.local()->IsFunction())throw std::runtime_error(std::string(stage)+" (JavaScript function missing)");
        std::vector<v8::Local<v8::Value>> values;values.reserve(args.size());
        for(const auto* arg:args)values.push_back(arg->local());
        begin();v8::Local<v8::Value> result;
        const bool ok=fn.local().As<v8::Function>()->Call(ctx,self.local(),static_cast<int>(values.size()),values.data()).ToLocal(&result);
        const bool timed=end();
        if(!ok)throw_failure(stage,timed,catcher);
        if(timed)throw std::runtime_error("Player JavaScript exceeded its time limit");
        return Value(this,result);
    }
};

Value::Value(Engine* e,v8::Local<v8::Value> value):engine(e),handle(e->isolate,value){}
v8::Local<v8::Value> Value::local() const{return handle.Get(engine->isolate);}
Value Value::get(const char* key) const {
    auto* isolate=engine->isolate;v8::Isolate::Scope isolate_scope(isolate);v8::HandleScope handles(isolate);
    auto ctx=engine->local_context();v8::Context::Scope context_scope(ctx);v8::Local<v8::Value> result;
    if(!local()->IsObject()||!local().As<v8::Object>()->Get(ctx,v8::String::NewFromUtf8(isolate,key).ToLocalChecked()).ToLocal(&result))
        throw std::runtime_error("Cannot read player JavaScript property");
    return Value(engine,result);
}
Value Value::at(uint32_t index) const {
    auto* isolate=engine->isolate;v8::Isolate::Scope isolate_scope(isolate);v8::HandleScope handles(isolate);
    auto ctx=engine->local_context();v8::Context::Scope context_scope(ctx);v8::Local<v8::Value> result;
    if(!local()->IsObject()||!local().As<v8::Object>()->Get(ctx,index).ToLocal(&result))throw std::runtime_error("Cannot read player JavaScript array");
    return Value(engine,result);
}
void Value::set_number(const char* key,int value) const {
    auto* isolate=engine->isolate;v8::Isolate::Scope isolate_scope(isolate);v8::HandleScope handles(isolate);
    auto ctx=engine->local_context();v8::Context::Scope context_scope(ctx);
    v8::Maybe<bool> result=local().As<v8::Object>()->Set(ctx,v8::String::NewFromUtf8(isolate,key).ToLocalChecked(),v8::Integer::New(isolate,value));
    if(result.IsNothing()||!result.FromJust())throw std::runtime_error("Cannot configure player JavaScript parser");
}
uint32_t Value::number() const {
    auto* isolate=engine->isolate;v8::Isolate::Scope isolate_scope(isolate);v8::HandleScope handles(isolate);
    return local()->Uint32Value(engine->local_context()).FromMaybe(0);
}
uint32_t Value::size() const{return get("length").number();}
std::string Value::text() const {
    auto* isolate=engine->isolate;v8::Isolate::Scope isolate_scope(isolate);v8::HandleScope handles(isolate);
    auto ctx=engine->local_context();v8::Context::Scope context_scope(ctx);auto value=local();
    if(value->IsNullOrUndefined())return {};
    v8::Local<v8::String> string;if(!value->ToString(ctx).ToLocal(&string))return {};
    v8::String::Utf8Value utf8(isolate,string);return *utf8?std::string(*utf8,utf8.length()):std::string{};
}
std::string Value::type() const{return get("type").text();}

// Acorn's offsets are UTF-16 positions; source strings in C++ are UTF-8.
std::vector<size_t> byte_offsets(const std::string& source) {
    std::vector<size_t> out;out.reserve(source.size()+1);
    for(size_t i=0;i<source.size();) {
        const unsigned char c=source[i];size_t n=c<128?1:c<224?2:c<240?3:4;
        out.push_back(i);if(n==4)out.push_back(i);i+=n;
    }
    out.push_back(source.size());return out;
}

bool is_entry(const Value& function) {
    if(function.type()!="FunctionExpression"&&function.type()!="FunctionDeclaration")return false;
    auto statements=function.get("body").get("body");
    for(uint32_t i=0;i<statements.size();++i) {
        auto s=statements.at(i);if(s.type()!="ExpressionStatement")continue;
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
    Engine engine(canceled,384*1024*1024);engine.reset(60);
    engine.eval(std::string(reinterpret_cast<const char*>(cryget_acorn_source),cryget_acorn_size),"JavaScript parser initialization failed");
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
    auto input=engine.string(source);auto options=engine.object();
    options.set_number("ecmaVersion",2026);
    log_event("player.parse.start","bytes="+std::to_string(source.size()));
    auto ast=engine.call(parse,acorn,{&input,&options},"Cannot parse player JavaScript");
    log_event("player.parse.ready","AST available");
    auto statements=ast.get("body");
    if(statements.size()<1||statements.size()>2)throw std::runtime_error("Unrecognized player script wrapper");
    auto statement=statements.at(statements.size()-1);auto expression=statement.get("expression");auto callee=expression.get("callee");
    if(callee.type()=="MemberExpression")callee=callee.get("object");
    if(callee.type()!="FunctionExpression")throw std::runtime_error("Unrecognized player function wrapper");
    auto block=callee.get("body");auto body=block.get("body");auto offsets=byte_offsets(source);
    auto pos=[&](const Value& node,const char* key){const auto i=node.get(key).number();if(i>=offsets.size())throw std::runtime_error("Invalid player source position");return offsets[i];};
    auto code=[&](const Value& node){auto a=pos(node,"start"),b=pos(node,"end");if(b<a)throw std::runtime_error("Invalid player source range");return source.substr(a,b-a);};
    std::vector<std::string> entries;std::string prepared=environment;prepared+=source.substr(0,pos(block,"start")+1);
    for(uint32_t i=0;i<body.size();++i) {
        if(canceled&&canceled->load())throw std::runtime_error("Download canceled");
        auto s=body.at(i);const auto type=s.type();
        if(type=="ExpressionStatement") {
            auto expr=s.get("expression");const auto et=expr.type();
            if(et!="AssignmentExpression"&&et!="Literal")continue;
            if(et=="AssignmentExpression"&&is_entry(expr.get("right")))entries.push_back(code(expr.get("left")));
        } else if(type=="VariableDeclaration") {
            auto declarations=s.get("declarations");
            if(i==0&&declarations.size()==1&&declarations.at(0).get("id").get("name").text()=="window") {prepared+="var window=globalThis;\n";continue;}
            for(uint32_t d=0;d<declarations.size();++d){auto decl=declarations.at(d);if(is_entry(decl.get("init")))entries.push_back(code(decl.get("id")));}
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
    Impl(const std::string& prepared,const std::atomic<bool>* canceled):engine(canceled){engine.eval(prepared,"Player initialization failed");}
};
PlayerSolver::PlayerSolver(const std::string& prepared,const std::atomic<bool>* canceled):impl(std::make_unique<Impl>(prepared,canceled)){}
PlayerSolver::~PlayerSolver()=default;

PlayerResult PlayerSolver::solve(const std::string& signature,const std::string& n) {
    auto key=std::make_pair(signature,n);const auto found=impl->cache.find(key);if(found!=impl->cache.end())return found->second;
    auto& e=impl->engine;e.reset();auto global=e.global();auto solvers=global.get("__crygetSolvers");
    PlayerResult result;bool obtained=false;
    for(uint32_t i=0;i<solvers.size();++i) {
        v8::Isolate::Scope isolate_scope(e.isolate);v8::HandleScope handles(e.isolate);
        v8::Context::Scope context_scope(e.local_context());
        try {
            auto fn=solvers.at(i);auto dummy=e.string("https://www.youtube.com/watch?v=aaaaaaaaaaa");
            auto param=e.string("s");auto sig=e.string(url_encode(signature));auto undefined=e.undefined();
            auto url=e.call(fn,global,{&dummy,&param,signature.empty()?&undefined:&sig},"Player signature transformation failed");
            if(!n.empty()) {
                auto set=url.get("set");auto nk=e.string("n");auto nv=e.string(n);
                e.call(set,url,{&nk,&nv},"Player n parameter setup failed");
                auto prototype=url.local().As<v8::Object>()->GetPrototype();
                if(prototype.IsEmpty()||!prototype->IsObject())throw std::runtime_error("Cannot read player URL methods");
                v8::Local<v8::Array> properties;
                if(!prototype.As<v8::Object>()->GetOwnPropertyNames(e.local_context()).ToLocal(&properties))throw std::runtime_error("Cannot read player URL methods");
                std::string transform;
                for(uint32_t k=0;k<properties->Length();++k) {
                    v8::Local<v8::Value> property;if(!properties->Get(e.local_context(),k).ToLocal(&property))continue;
                    v8::String::Utf8Value utf8(e.isolate,property);std::string name=*utf8?std::string(*utf8,utf8.length()):std::string{};
                    if(name!="constructor"&&name!="set"&&name!="get"&&name!="clone"&&!name.empty()){transform=name;break;}
                }
                if(transform.empty())throw std::runtime_error("Player n transformation method is missing");
                auto method=url.get(transform.c_str());e.call(method,url,{},"Player n transformation failed");
            }
            auto get=url.get("get");auto sk=e.string("s");auto nk=e.string("n");PlayerResult current;
            if(!signature.empty())current.signature=url_decode(e.call(get,url,{&sk},"Cannot read player signature").text());
            if(!n.empty())current.n=e.call(get,url,{&nk},"Cannot read player n parameter").text();
            if((!signature.empty()&&current.signature.empty())||(!n.empty()&&(current.n.empty()||current.n.find("enhanced_except_")==0)))throw std::runtime_error("Player returned an invalid signature");
            if(obtained&&(result.signature!=current.signature||result.n!=current.n))throw std::logic_error("Player signature functions returned conflicting results");
            result=current;obtained=true;
        } catch(const std::runtime_error&) {if(solvers.size()==1)throw;}
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
        if(!signature.empty()){auto name=query_value(cipher,"sp");url=set_url_parameter(url,name.empty()?"signature":name,result.signature);}
        if(!n.empty())url=set_url_parameter(url,"n",result.n);
    }
    return url;
}
} // namespace cryget
