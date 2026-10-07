#include "player.hpp"
#include "url_utils.hpp"
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <chrono>

static void require(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
int main(int argc,char** argv){try{
    // Offline, authored fixture: includes removed top-level browser calls, regex,
    // Unicode source offsets, a declaration after offset 256, and prototype methods.
    std::string source=R"JS(var p={};(function(g){
      var window=this; var unicode='标题😀';
      g.Url=function(){this.values={};};
      g.Url.prototype.set=function(k,v){this.values[k]=v};
      g.Url.prototype.get=function(k){return this.values[k]};
      g.Url.prototype.clone=function(){return this};
      g.Url.prototype.transform=function(){if(this.values.n)this.values.n=this.values.n.split('').reverse().join('')};
      browserInitializationThatMustNotRun();
      var decipher=function(url,key,sig){url=new g.Url();url.set('alr','yes');
        if(sig)url.set(key,encodeURIComponent(decodeURIComponent(sig).slice(1).split('').reverse().join('')));
        return url;
      };
    })(p);)JS";
    cryget::PlayerSolver solver(cryget::PlayerSolver::prepare(source));
    auto r=solver.solve("aBC+%/", "123xyz");
    require(r.signature=="/%+CB"&&r.n=="zyx321","signature / n transformation failed");
    const auto cipher="url="+cryget::url_encode("https://media.example/video?keep=a%2Fb&n=123xyz#end")+"&sp=sig&s="+cryget::url_encode("aBC+%/");
    const auto url=cryget::resolve_media_url("",cipher,&solver);
    require(cryget::url_parameter(url,"sig")=="/%+CB"&&cryget::url_parameter(url,"n")=="zyx321","cipher query encoding failed");
    require(url.find("keep=a%2Fb")!=std::string::npos&&url.substr(url.size()-4)=="#end","unrelated URL parameters changed");
    auto wrapped=source.substr(source.find("(function"));
    wrapped.replace(wrapped.rfind(")(p);"),5,").call(this,{});");
    cryget::PlayerSolver other(cryget::PlayerSolver::prepare(wrapped));
    require(other.solve("abc","123").signature=="cb","single-statement wrapper failed");
    bool rejected=false;try{cryget::PlayerSolver::prepare("function unsupported(){}");}catch(const std::runtime_error&){rejected=true;}require(rejected,"unknown wrapper accepted");
    std::atomic<bool> canceled{true};rejected=false;
    try{cryget::PlayerSolver stopped(cryget::PlayerSolver::prepare(source,&canceled),&canceled);stopped.solve("abc","123");}catch(const std::runtime_error&){rejected=true;}require(rejected,"JS cancellation failed");
    // The embedded engine exposes no host filesystem, network or process APIs.
    const auto guard="if(typeof require!=='undefined'||typeof fetch!=='undefined'||typeof std!=='undefined'||typeof os!=='undefined')throw Error('host API');";
    cryget::PlayerSolver sandbox(guard+cryget::PlayerSolver::prepare(source));
    require(sandbox.solve("abc","123").n=="321","sandbox setup failed");
    auto infinite=source;auto at=infinite.find("if(this.values.n)");infinite.insert(at,"while(true){};");
    cryget::PlayerSolver bounded(cryget::PlayerSolver::prepare(infinite));
    auto start=std::chrono::steady_clock::now();rejected=false;
    try{bounded.solve("abc","123");}catch(const std::runtime_error&){rejected=true;}
    require(rejected&&std::chrono::steady_clock::now()-start<std::chrono::seconds(6),"JS execution time limit failed");
    if(argc>1){std::ifstream file(argv[1]);std::ostringstream content;content<<file.rdbuf();
        cryget::PlayerSolver real(cryget::PlayerSolver::prepare(content.str()));
        const std::string sample="5QjJrWzVcOutYYNyxkDJVkzQDZQxNbbxGi4hRoh2h4PomQMQq9vo2WPHVpHgxRn7qT3WyhRiJa1k1t1DL3lynZtupHmG3wW4qh59faKjtY4UVu";
        auto actual=real.solve(sample,"abcdefghijklmnopqrstuvwxyz");
        require(!actual.signature.empty()&&actual.signature!=sample&&!actual.n.empty(),"real player transformation failed");
        std::cout<<"real player: signature and n transformed\n";
    }
    std::cout<<"player tests passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
