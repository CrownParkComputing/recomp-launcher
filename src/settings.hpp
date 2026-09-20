#pragma once
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <string>
#include <stdexcept>
#include <cstdlib>
namespace fs=std::filesystem;
struct Settings {
    int fps=60, mode=0, show=1, renderer=0; // renderer: 0 OpenGL(raylib), 1 Vulkan
    std::string saves, pad;
};
inline bool valid_fps(int n){return n==30||n==60||n==90||n==120;}
inline bool valid_renderer(int r){return r==0||r==1;}
inline fs::path base_dir(const char *env,const char *suffix){
    const char *p=getenv(env);if(p&&fs::path(p).is_absolute())return p;
    const char *home=getenv("HOME");if(!home)throw std::runtime_error("HOME is unset");
    return fs::path(home)/suffix;
}
inline void save_settings(const fs::path &path,const Settings &s){
    if(!valid_fps(s.fps)||s.mode<0||s.mode>2||!valid_renderer(s.renderer)||!fs::path(s.saves).is_absolute())
        throw std::runtime_error("Choose an absolute save folder and valid settings");
    fs::create_directories(path.parent_path());
    auto tmp=path;tmp += ".tmp";
    // renderer trails the line so pre-renderer conf files still load.
    std::ofstream f(tmp);f<<s.fps<<' '<<s.mode<<' '<<s.show<<' '<<std::quoted(s.saves)<<' '<<std::quoted(s.pad)<<' '<<s.renderer<<'\n';
    f.close();if(!f)throw std::runtime_error("Cannot write settings");fs::rename(tmp,path);
}
inline Settings load_settings(const fs::path &path,Settings defaults){
    std::ifstream f(path);if(!f)return defaults;
    Settings s;
    if(!(f>>s.fps>>s.mode>>s.show>>std::quoted(s.saves)>>std::quoted(s.pad))||!valid_fps(s.fps)||s.mode<0||s.mode>2||!fs::path(s.saves).is_absolute())
        throw std::runtime_error("Invalid settings: "+path.string());
    int r=0;if((f>>r)&&valid_renderer(r))s.renderer=r;else s.renderer=0;
    return s;
}
inline fs::path prepare_save(const Settings &s,const fs::path &old){
    if(!fs::path(s.saves).is_absolute())throw std::runtime_error("Save folder must be absolute");
    fs::create_directories(s.saves);
    fs::path dst=fs::path(s.saves)/"vmu_a1.bin";
    if(!fs::exists(dst)&&fs::exists(old))fs::copy_file(old,dst,fs::copy_options::skip_existing);
    if(fs::exists(dst)&&fs::file_size(dst)!=128*1024)throw std::runtime_error("VMU has the wrong size; original file left untouched");
    return dst;
}
