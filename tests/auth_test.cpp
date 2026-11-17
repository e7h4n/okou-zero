// SPDX-License-Identifier: MIT
#include "okou/core.hpp"
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <openssl/evp.h>
#include <filesystem>
#include <thread>
#include <mutex>
#include <iostream>
#include <ctime>

#define CHECK(x) do{if(!(x))throw std::runtime_error("check failed: " #x);}while(false)
namespace {
using okou::Json;
struct Reply {int status;Json body;};
class Fixture {
    int fd_;std::thread thread_;std::vector<Reply> replies_;std::mutex mutex_;std::vector<std::string> requests_;
public:
    std::string base;
    Fixture(std::vector<Reply> replies):replies_(std::move(replies)) {
        fd_=::socket(AF_INET,SOCK_STREAM,0);CHECK(fd_>=0);
        sockaddr_in addr{};addr.sin_family=AF_INET;addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
        CHECK(::bind(fd_,reinterpret_cast<sockaddr*>(&addr),sizeof addr)==0);CHECK(::listen(fd_,16)==0);
        socklen_t len=sizeof addr;CHECK(::getsockname(fd_,reinterpret_cast<sockaddr*>(&addr),&len)==0);
        base="http://127.0.0.1:"+std::to_string(ntohs(addr.sin_port));
        thread_=std::thread([this]{
            for(const auto& r:replies_) {
                int c=::accept(fd_,nullptr,nullptr);if(c<0)break;
                timeval timeout{3,0};::setsockopt(c,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof timeout);
                std::string request;char bytes[4096];std::size_t end=std::string::npos,size=0;
                while(true){auto n=::recv(c,bytes,sizeof bytes,0);if(n<=0)break;request.append(bytes,n);end=request.find("\r\n\r\n");if(end!=std::string::npos){auto p=request.find("Content-Length:");if(p!=std::string::npos){try{size=std::stoul(request.substr(p+15));}catch(...){}}if(request.size()>=end+4+size)break;}}
                {std::lock_guard<std::mutex> lock(mutex_);requests_.push_back(request);}
                auto body=r.body.dump();for(std::size_t p=0;(p=body.find("$BASE",p))!=std::string::npos;p+=base.size())body.replace(p,5,base);
                auto response="HTTP/1.1 "+std::to_string(r.status)+" Fixture\r\nConnection: close\r\nContent-Type: application/json\r\nContent-Length: "+std::to_string(body.size())+"\r\n\r\n"+body;
                for(std::size_t p=0;p<response.size();) {auto n=::send(c,response.data()+p,response.size()-p,MSG_NOSIGNAL);if(n<=0)break;p+=static_cast<std::size_t>(n);}::close(c);
            }
        });
    }
    std::vector<std::string> requests(){std::lock_guard<std::mutex> lock(mutex_);return requests_;}
    ~Fixture(){::shutdown(fd_,SHUT_RDWR);::close(fd_);if(thread_.joinable())thread_.join();}
};
Json metadata() {return {{"issuer","$BASE"},{"device_authorization_endpoint","$BASE/device"},{"token_endpoint","$BASE/token"},{"revocation_endpoint","$BASE/revoke"},{"token_endpoint_auth_methods_supported",{"none"}},{"grant_types_supported",{"urn:ietf:params:oauth:grant-type:device_code","refresh_token"}}};}
Json device() {return {{"device_code","private-device"},{"user_code","SAFE-CODE"},{"verification_uri","$BASE/activate"},{"verification_uri_complete","$BASE/activate?user_code=SAFE-CODE"},{"expires_in",600},{"interval",7}};}
std::string b64(const std::string& data) {
    std::string result(4*((data.size()+2)/3),'\0');EVP_EncodeBlock(reinterpret_cast<unsigned char*>(result.data()),reinterpret_cast<const unsigned char*>(data.data()),static_cast<int>(data.size()));
    while(!result.empty()&&result.back()=='=')result.pop_back();
    for(char& c:result){if(c=='+')c='-';if(c=='/')c='_';}
    return result;
}
Json token(const std::string& org="org_test",const std::string& refresh="refresh-old") {
    Json claims={{"sub","user_test"},{"org_id",org},{"client_id","public-test"},{"exp",std::time(nullptr)+3600}};
    Json t={{"access_token",b64("{\"typ\":\"at+jwt\"}")+"."+b64(claims.dump())+".fake-signature"},{"token_type","Bearer"},{"expires_in",3000}};
    if(!refresh.empty())t["refresh_token"]=refresh;
    return t;
}
template<class F> std::string fails(F f){try{f();}catch(const std::exception& e){return e.what();}throw std::runtime_error("expected failure");}
okou::Config config(Fixture& f,const std::string& dir) {okou::Config c;c.api_origin=f.base;c.oauth_issuer=f.base;c.oauth_client_id="public-test";c.data_dir=dir;return c;}
void put(const std::string& path,const std::string& value,mode_t mode=0600){int fd=::open(path.c_str(),O_WRONLY|O_CREAT|O_TRUNC,mode);CHECK(fd>=0);CHECK(::write(fd,value.data(),value.size())==static_cast<ssize_t>(value.size()));CHECK(::close(fd)==0);CHECK(::chmod(path.c_str(),mode)==0);}
void tests() {
    ::setenv("OKOU_ZERO_TEST_ALLOW_LOOPBACK_HTTP","1",1);char temp[]="/tmp/okou-auth-test-XXXXXX";CHECK(::mkdtemp(temp));std::string root=temp;
    ::setenv("XDG_CONFIG_HOME",(root+"/config").c_str(),1);::setenv("XDG_DATA_HOME",(root+"/data").c_str(),1);
    ::unsetenv("OKOU_ZERO_CONFIG");::unsetenv("OKOU_ZERO_DATA_DIR");
    okou::Http http;
    {
        Fixture f({{200,metadata()},{200,device()},{400,{{"error","authorization_pending"},{"error_description","PRIVATE provider text"}}},
            {400,{{"error","slow_down"}}},{400,{{"error","access_denied"}}},{400,{{"error","expired_token"}}},{200,token()},
            {200,token("org_test","refresh-rotated")},{200,token("org_test","")},{503,{{"error","PRIVATE"}}}});
        auto c=config(f,root+"/credentials");okou::OAuth auth(c,http);auto d=auth.begin();CHECK(d.interval==7);CHECK(d.expires_in==600);CHECK(d.user_code=="SAFE-CODE");
        CHECK(auth.poll(d).state==okou::PollState::pending);CHECK(auth.poll(d).state==okou::PollState::slow_down);CHECK(auth.poll(d).state==okou::PollState::denied);CHECK(auth.poll(d).state==okou::PollState::expired);
        auto result=auth.poll(d);CHECK(result.state==okou::PollState::authorized);auto t=result.tokens;CHECK(t.user_id=="user_test");CHECK(t.org_id=="org_test");CHECK(t.client_id=="public-test");CHECK(t.expires_at<=std::time(nullptr)+3000);
        CHECK(auth.load().access_token.empty());auth.save(t);auto loaded=auth.load();CHECK(loaded.access_token==t.access_token);CHECK(loaded.org_id=="org_test");CHECK(loaded.expires_at==t.expires_at);
        struct stat st{};CHECK(::stat((c.data_dir+"/tokens.json").c_str(),&st)==0);CHECK((st.st_mode&0777)==0600);CHECK(::stat(c.data_dir.c_str(),&st)==0);CHECK((st.st_mode&0777)==0700);
        Json tampered={{"access_token",t.access_token},{"refresh_token",t.refresh_token},{"org_id","org_attacker"},{"user_id","user_attacker"},{"expires_at",std::time(nullptr)+999999}};
        put(c.data_dir+"/tokens.json",tampered.dump());CHECK(auth.load().org_id=="org_test");CHECK(auth.load().user_id=="user_test");CHECK(auth.load().expires_at<=std::time(nullptr)+3600);
        auto rotated=auth.refresh(t);CHECK(rotated.refresh_token=="refresh-rotated");auto retained=auth.refresh(rotated);CHECK(retained.refresh_token=="refresh-rotated");auth.save(retained);
        auth.logout(retained);CHECK(auth.load().access_token.empty());
        auto requests=f.requests();CHECK(requests.size()==10);CHECK(requests[1].find("client_id=public-test")!=std::string::npos);CHECK(requests[1].find("resource=https%3A%2F%2Fapi.okou.ai%2Fmcp")!=std::string::npos);
        for(const auto& request:requests){CHECK(request.find("client_secret")==std::string::npos);CHECK(request.find("organization_id")==std::string::npos);CHECK(request.find("Authorization:")==std::string::npos);}
    }
    {
        auto m=metadata();Fixture f({{200,{{"authorization_servers",{"$BASE"}},{"resource","https://api.okou.ai/mcp"}}},{404,{{"error","not_found"}}},{200,m},{200,device()}});
        auto c=config(f,root+"/discovery");c.oauth_issuer.clear();okou::OAuth auth(c,http);CHECK(auth.begin().device_code=="private-device");auto r=f.requests();CHECK(r[0].find("GET /.well-known/oauth-protected-resource/mcp ")!=std::string::npos);CHECK(r[2].find("GET /.well-known/oauth-authorization-server ")!=std::string::npos);
    }
    {
        Fixture f({{200,metadata()},{200,device()},{200,token("")}});okou::OAuth auth(config(f,root+"/org"),http);auto d=auth.begin();CHECK(fails([&]{auth.poll(d);}).find("Org-bound")!=std::string::npos);
    }
    {
        Fixture f({{200,metadata()},{200,device()},{400,{{"error","invalid_client"},{"error_description","PRIVATE client secret"}}}});okou::OAuth auth(config(f,root+"/error"),http);auto d=auth.begin();CHECK(fails([&]{auth.poll(d);}).find("PRIVATE")==std::string::npos);
    }
    {
        auto m=metadata();m["issuer"]="https://wrong.example";Fixture f({{200,m}});okou::OAuth auth(config(f,root+"/issuer"),http);CHECK(fails([&]{auth.begin();}).find("issuer mismatch")!=std::string::npos);
    }
    {
        auto m=metadata();m["token_endpoint_auth_methods_supported"]={"client_secret_basic"};Fixture f({{200,m}});okou::OAuth auth(config(f,root+"/public"),http);CHECK(fails([&]{auth.begin();}).find("public clients")!=std::string::npos);
    }
    {
        auto m=metadata();m["grant_types_supported"]={"authorization_code"};Fixture f({{200,m}});okou::OAuth auth(config(f,root+"/grant"),http);CHECK(fails([&]{auth.begin();}).find("device grant")!=std::string::npos);
    }
    {
        Fixture f({});auto c=config(f,root+"/missing");c.oauth_client_id.clear();okou::OAuth auth(c,http);CHECK(fails([&]{auth.begin();}).find("CLIENT_ID")!=std::string::npos);
    }
    {
        Fixture f({});auto c=config(f,root+"/cancel");std::atomic_bool cancelled{true};okou::Http hc(&cancelled);okou::OAuth auth(c,hc);
        okou::Tokens t;t.access_token="private";auth.save(t);auth.logout(t);CHECK(auth.load().access_token.empty());
    }
    {
        auto c=okou::load_config();CHECK(c.data_dir==root+"/data/okou-zero");c.oauth_client_id="configured-public";c.oauth_issuer="https://issuer.example";okou::save_config(c);
        CHECK(okou::load_config().oauth_client_id=="configured-public");::setenv("OKOU_ZERO_OAUTH_CLIENT_ID","environment-public",1);CHECK(okou::load_config().oauth_client_id=="environment-public");::unsetenv("OKOU_ZERO_OAUTH_CLIENT_ID");
        put(root+"/explicit.json","{\"oauth_client_id\":\"explicit\"}");CHECK(okou::load_config(root+"/explicit.json").oauth_client_id=="explicit");
        ::chmod((root+"/explicit.json").c_str(),0644);CHECK(fails([&]{okou::load_config(root+"/explicit.json");}).find("permissions")!=std::string::npos);
        ::chmod((root+"/explicit.json").c_str(),0600);put(root+"/explicit.json","{ PRIVATE SECRET");CHECK(fails([&]{okou::load_config(root+"/explicit.json");}).find("PRIVATE")==std::string::npos);
        CHECK(::symlink((root+"/explicit.json").c_str(),(root+"/symlink.json").c_str())==0);CHECK(!fails([&]{okou::load_config(root+"/symlink.json");}).empty());
        CHECK(::symlink(root.c_str(),(root+"/linkdir").c_str())==0);CHECK(!fails([&]{okou::load_config(root+"/linkdir/explicit.json");}).empty());
        put(root+"/source.json","{}");CHECK(::link((root+"/source.json").c_str(),(root+"/hardlink.json").c_str())==0);CHECK(!fails([&]{okou::load_config(root+"/hardlink.json");}).empty());
        CHECK(::mkdir((root+"/unsafe").c_str(),0755)==0);CHECK(!fails([&]{okou::load_config(root+"/unsafe/config.json");}).empty());
        CHECK(::mkfifo((root+"/fifo").c_str(),0600)==0);CHECK(!fails([&]{okou::load_config(root+"/fifo");}).empty());
    }
    std::filesystem::remove_all(root);
}
}
int main(){try{tests();std::cout<<"auth_test passed\n";return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
