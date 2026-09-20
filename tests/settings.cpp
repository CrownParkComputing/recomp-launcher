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

    // Save/load round trip, quoted strings included.
    Settings s;
    s.fps=120;s.mode=2;s.show=0;
    s.saves="/tmp/recomp-settings-test/my saves";
    s.pad="Microsoft Xbox Series S|X Controller";
    const fs::path conf=cfg/"powerstone.conf";
    save_settings(conf,s);
    Settings t=load_settings(conf,Settings{});
    assert(t.fps==120&&t.mode==2&&t.show==0);
    assert(t.saves==s.saves&&t.pad==s.pad);

    // Missing file returns the defaults handed in.
    Settings d=load_settings(cfg/"not-there.conf",s);
    assert(d.fps==120&&d.saves==s.saves);

    // Invalid values are rejected, not clamped.
    assert(!valid_fps(45)&&valid_fps(30)&&valid_fps(90));
    Settings bad=s;bad.fps=45;
    bool threw=false;
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
