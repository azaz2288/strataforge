#include "engine.hpp"
#include <iostream>
#include <sstream>

static std::string unhex(const std::string& text) {
    if (text == "-") return "";
    strata::require(text.size() % 2 == 0 && text.size() <= 2U * strata::max_frame, "invalid hex length");
    auto digit=[](char c)->unsigned { if(c>='0'&&c<='9')return static_cast<unsigned>(c-'0'); if(c>='a'&&c<='f')return static_cast<unsigned>(c-'a'+10); throw std::runtime_error("invalid hex"); };
    std::string out; for(std::size_t i=0;i<text.size();i+=2)out.push_back(static_cast<char>(digit(text[i])*16+digit(text[i+1]))); return out;
}
static std::string hex(const std::string& text) {
    if(text.empty())return "-";
    const char* digits="0123456789abcdef"; std::string out; for(unsigned char c:text){out+=digits[c>>4];out+=digits[c&15];} return out;
}
#ifdef _WIN32
int wmain(int argc,wchar_t** argv) {
#else
int main(int argc,char** argv) {
#endif
    try {
        strata::require(argc==2,"usage: strataforge DATABASE_DIRECTORY"); strata::Engine db(argv[1]); std::string line;
        std::cout<<"READY "<<db.sequence()<<std::endl;
        while(std::getline(std::cin,line)) {
            strata::require(line.size()<=2U*strata::max_frame+100,"command too large");
            std::istringstream input(line); std::string command; input>>command;
            if(command=="batch") {
                std::vector<strata::Mutation> mutations; std::string key,value;
                while(input>>key){strata::require(static_cast<bool>(input>>value),"missing batch value");mutations.push_back({unhex(key),value=="~"?std::nullopt:std::optional<std::string>(unhex(value))});}
                auto sequence=db.batch(mutations); std::cout<<"OK "<<sequence<<std::endl;
            } else if(command=="get") {
                std::uint64_t at; std::string key; strata::require(static_cast<bool>(input>>at>>key),"get SEQUENCE HEXKEY"); auto value=db.get(unhex(key),at); std::cout<<(value?"VALUE "+hex(*value):"MISSING")<<std::endl;
            } else if(command=="scan") {
                std::uint64_t at; std::string lo,hi; strata::require(static_cast<bool>(input>>at>>lo>>hi),"scan SEQUENCE LOW HIGH");
                auto rows=db.scan(unhex(lo),unhex(hi),at); std::cout<<"ROWS "<<rows.size(); for(const auto& [key,value]:rows)std::cout<<' '<<hex(key)<<' '<<hex(value); std::cout<<std::endl;
            } else if(command=="flush"){db.flush();std::cout<<"OK "<<db.sequence()<<std::endl;}
            else if(command=="compact"){db.compact();std::cout<<"OK "<<db.sequence()<<std::endl;}
            else if(command=="snapshot"){std::cout<<"SEQ "<<db.sequence()<<std::endl;}
            else if(command=="quit"){return 0;}
            else throw std::runtime_error("unknown command");
        }
        return 0;
    } catch(const std::exception& e){std::cerr<<"ERROR "<<e.what()<<std::endl;return 2;}
}
