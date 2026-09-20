// Round-trip and validation tests for settings.hpp. Built by `make test`.
#include "settings.hpp"
#include <cassert>
#include <cstdio>
#include <cstdlib>

int main(){
    setenv("HOME","/tmp/recomp-settings-test",1);
    unsetenv("XDG_CONFIG_HOME");
    unsetenv("XDG_DATA_HOME");
    const fs::path cfg=base_dir("XDG_CONFIG_HOME",".config")/"recomp-launcher";
    std::error_code ec;
    fs::remove_all("/tmp/recomp-settings-test",ec);

    // XDG fallback lands under HOME.
    assert(base_dir("XDG_CONFIG_HOME",".config")==fs::path("/tmp/recomp-settings-test/.config"));

    // Save/load round trip, quoted strings + graphics fields included.
    Settings s;
    s.fps=120;s.mode=2;s.show=0;s.renderer=1;
    s.msaa=8;s.resolution=150;s.aniso=4;
    s.saves="/tmp/recomp-settings-test/my saves";
    s.pad="Microsoft Xbox Series S|X Controller";
    const fs::path conf=cfg/"powerstone.conf";
    save_settings(conf,s);
    Settings t=load_settings(conf,Settings{});
    assert(t.fps==120&&t.mode==2&&t.show==0&&t.renderer==1);
    assert(t.msaa==8&&t.resolution==150&&t.aniso==4);
    assert(t.saves==s.saves&&t.pad==s.pad);

    // Pre-renderer conf files (5 fields) still load; renderer + graphics default.
    {std::ofstream f(conf);f<<"60 0 1 \"/tmp/recomp-settings-test/my saves\" \"\"  \n";}
    Settings r=load_settings(conf,Settings{});
    assert(r.fps==60&&r.renderer==0&&r.msaa==4&&r.resolution==100&&r.aniso==1);

    // Pre-graphics conf files (6 fields, with renderer) still load; graphics default.
    {std::ofstream f(conf);f<<"60 0 1 \"/tmp/recomp-settings-test/my saves\" \"\" 1\n";}
    r=load_settings(conf,Settings{});
    assert(r.fps==60&&r.renderer==1&&r.msaa==4&&r.resolution==100&&r.aniso==1);

    // An out-of-range stored renderer falls back, not crashes.
    {std::ofstream f(conf);f<<"60 0 1 \"/tmp/recomp-settings-test/my saves\" \"\" 7\n";}
    r=load_settings(conf,Settings{});
    assert(r.renderer==0);

    // Out-of-range MSAA / resolution / aniso in the file fall back too.
    {std::ofstream f(conf);f<<"60 0 1 \"/tmp/recomp-settings-test/my saves\" \"\" 0 99 33 3\n";}
    r=load_settings(conf,Settings{});
    assert(r.msaa==4&&r.resolution==100&&r.aniso==1);

    // Missing file returns the defaults handed in.
    Settings d=load_settings(cfg/"not-there.conf",s);
    assert(d.fps==120&&d.saves==s.saves);

    // Invalid values are rejected, not clamped.
    assert(!valid_fps(45)&&valid_fps(30)&&valid_fps(90));
    assert(!valid_msaa(3)&&valid_msaa(1)&&valid_msaa(8));
    assert(!valid_resolution(80)&&valid_resolution(50)&&valid_resolution(200));
    assert(!valid_aniso(3)&&valid_aniso(1)&&valid_aniso(16));
    Settings bad=s;bad.fps=45;
    bool threw=false;
    try{save_settings(conf,bad);}catch(const std::exception&){threw=true;}
    assert(threw);
    bad=s;bad.msaa=3;
    threw=false;
    try{save_settings(conf,bad);}catch(const std::exception&){threw=true;}
    assert(threw);

    // Corrupt file throws instead of loading garbage.
    {std::ofstream f(conf);f<<"this is not a settings file\n";}
    threw=false;
    try{load_settings(conf,Settings{});}catch(const std::exception&){threw=true;}
    assert(threw);

    // prepare_save creates the folder; a missing source VMU is fine, a
    // wrong-sized destination is not overwritten silently.
    Settings v;v.saves="/tmp/recomp-settings-test/vmu";
    const fs::path dst=prepare_save(v,"/nonexistent/vmu_a1.bin");
    assert(fs::exists(dst.parent_path())&&!fs::exists(dst));
    {std::ofstream f(dst);f<<"too small";}
    threw=false;
    try{prepare_save(v,"/nonexistent/vmu_a1.bin");}catch(const std::exception&){threw=true;}
    assert(threw);

    fs::remove_all("/tmp/recomp-settings-test",ec);
    puts("settings tests ok");
    return 0;
}