// recomp-launcher - the library menu for the native Dreamcast recompiles,
// styled after rexmenu (the Xbox 360 family menu): same palette, same
// dashboard icon rail, same pad-first navigation.
//
//   recomp-launcher [--smoke <seconds>] [--launch <id>]
//
// The launcher has four screens, all reachable from the rail at the top of
// the Home screen:
//
//   Home      Play / Game files / Settings / About / Quit
//   Files     Import GDI image... / Clear imported data / Back
//   Settings  Frame-rate limit / FPS overlay / Renderer / Controller /
//             External pad device - global, auto-saved per change.
//   About     100% native details and per-game diagnostics for the rail
//             entry that was active when About was opened.
//
// Settings are global (one launcher.conf under ~/.config/recomp-launcher/),
// so a pad you picked on one game is still the pad on the next. Save data
// is per game: each game gets its own saves/<id>/ directory under the
// launcher's XDG data root, and the VMU file lives there. Play forks the
// game's own launch.sh with the recomp environment and logs to
// ~/.local/share/recomp-launcher/logs/; the menu waits for the child.
#include "raylib.h"
#include "settings.hpp"
#include <sys/wait.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <algorithm>
#include <array>
#include <string>
#include <vector>

// GLFW lives inside libraylib.a; its gamepad mapping database is what tells
// a real controller apart from a wireless mouse dongle (raylib's
// IsGamepadAvailable alone lists both).
extern "C" int glfwJoystickIsGamepad(int);

/* ---- palette (same colors as rexmenu) ----------------------------------- */
static const Color col_bg{16,18,22,255}, col_panel{28,32,40,255};
static const Color col_sel{40,110,120,255}, col_outline{80,210,220,255};
static const Color col_muted{130,140,155,255}, col_hint{110,120,135,255};
static const Color col_ok{120,200,140,255}, col_warn{220,170,90,255};
static const Color col_err{230,120,110,255};
static const Color col_tile{36,42,52,255}, col_letter{150,200,230,255};
static const Color col_teal{100,200,210,255};

/* ---- font: one good face, raylib's built-in only as the fallback -------- */
static Font ui_font{};
static bool ui_font_ok=false;
static void load_ui_font(){
    const char *const candidates[]={
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation2/LiberationSans-Regular.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
        "/usr/share/fonts/liberation/LiberationSans-Regular.ttf",
        "/usr/share/fonts/Adwaita/AdwaitaSans-Regular.ttf",
    };
    // ASCII plus the em dash the details block uses.
    std::vector<int> cps;
    for(int c=32;c<127;c++)cps.push_back(c);
    cps.push_back(0x2014);
    for(const char *path:candidates){
        if(access(path,R_OK)!=0)continue;
        Font f=LoadFontEx(path,48,cps.data(),(int)cps.size());
        // A failed LoadFontEx still hands back raylib's built-in font
        // (exactly 224 glyphs); treat that as "did not load".
        if(f.texture.width>8&&f.glyphCount!=224){ui_font=f;ui_font_ok=true;break;}
        UnloadFont(f);
    }
    if(ui_font_ok)SetTextureFilter(ui_font.texture,TEXTURE_FILTER_BILINEAR);
}
static void ui_text(const std::string &s,int x,int y,int size,Color c){
    if(ui_font_ok)DrawTextEx(ui_font,s.c_str(),{(float)x,(float)y},(float)size,1.0f,c);
    else DrawText(s.c_str(),x,y,size,c);
}
static float ui_measure(const std::string &s,int size){
    if(ui_font_ok)return MeasureTextEx(ui_font,s.c_str(),(float)size,1.0f).x;
    return (float)MeasureText(s.c_str(),size);
}
static void ui_text_center(const std::string &s,int cx,int y,int size,Color c){
    ui_text(s,cx-(int)(ui_measure(s,size)/2.0f),y,size,c);
}
// Shrink a path from the left until it fits (the tail is what identifies it).
static std::string fit_left(std::string s,int size,float width){
    while(ui_measure(s,size)>width&&s.size()>4)s.erase(0,1);
    return s;
}

/* ---- rail tile: a texture that outlives the file it came from ----------- */
// For a failed (or absent) load the rail draws a letter tile instead - a
// rail with holes is worse than one with initials.
class ScopedTexture {
public:
    ScopedTexture()=default;
    explicit ScopedTexture(const std::string &path){
        Image img=LoadImage(path.c_str());
        if(img.data!=nullptr){
            tex_=LoadTextureFromImage(img);
            UnloadImage(img);
            ok_=tex_.id!=0;
            if(ok_)SetTextureFilter(tex_,TEXTURE_FILTER_BILINEAR);
        }
    }
    ScopedTexture(ScopedTexture &&o)noexcept{move_from(o);}
    ScopedTexture &operator=(ScopedTexture &&o)noexcept{
        if(this!=&o){release();move_from(o);}
        return *this;
    }
    ScopedTexture(const ScopedTexture&)=delete;
    ScopedTexture &operator=(const ScopedTexture&)=delete;
    ~ScopedTexture(){release();}
    bool ok()const{return ok_;}
    Texture2D &tex(){return tex_;}
private:
    void move_from(ScopedTexture &o){tex_=o.tex_;ok_=o.ok_;o.tex_=Texture2D{};o.ok_=false;}
    void release(){if(tex_.id&&IsWindowReady())UnloadTexture(tex_);tex_=Texture2D{};ok_=false;}
    Texture2D tex_{};
    bool ok_=false;
};

/* ---- the games ----------------------------------------------------------- */
struct Game {
    const char *id,*title,*tag,*dir,*host;
    const char *how1,*how2; // how the port runs, drawn in the About block
    int max_fps=120;        // gs.fps is clamped to this at launch time
    pid_t pid=0;
    std::string log;
};

// Controller modes: 0 = external pad, 2 = keyboard. 1 was the virtual pad
// overlay, which the recomps no longer have - a legacy conf value of 1 is
// treated as 0 everywhere.
static const char *mode_label(int mode){return mode==2?"Keyboard":"External pad";}
static const char *mode_env(int mode){return mode==2?"keyboard":"external";}
static int fixed_mode(int mode){return mode==2?2:0;}

// FPS caps offered on the Settings row. Clamped to the active game's
// max_fps so MSR's 30Hz native cap can't be overridden by a global 120.
static int cycle_fps(int fps,int dir,int max_fps){
    const int rates[]={30,60,120};
    int valid[3],nv=0;
    for(int i=0;i<3;i++)if(rates[i]<=max_fps)valid[nv++]=rates[i];
    if(nv==0)return fps; // no legal option - leave as-is
    int at=0;for(int i=0;i<nv;i++)if(fps==valid[i])at=i;
    return valid[(at+dir+nv)%nv];
}

/* ---- input: one-frame edges from every pad plus the keyboard ------------ */
struct Nav {
    bool up=false,down=false,left=false,right=false,
         a=false,b=false,y=false,start=false,lb=false,rb=false;
    void consume(){up=down=left=right=a=b=y=false;start=lb=rb=false;}
};
#ifndef MAX_GAMEPADS
#define MAX_GAMEPADS 4
#endif
static void poll_input(Nav &nav){
    nav.consume();
    for(int gp=0;gp<MAX_GAMEPADS;++gp){
        if(!IsGamepadAvailable(gp))continue;
        if(IsGamepadButtonPressed(gp,GAMEPAD_BUTTON_LEFT_FACE_UP))nav.up=true;
        if(IsGamepadButtonPressed(gp,GAMEPAD_BUTTON_LEFT_FACE_DOWN))nav.down=true;
        if(IsGamepadButtonPressed(gp,GAMEPAD_BUTTON_LEFT_FACE_LEFT))nav.left=true;
        if(IsGamepadButtonPressed(gp,GAMEPAD_BUTTON_RIGHT_FACE_RIGHT))nav.b=true;
        if(IsGamepadButtonPressed(gp,GAMEPAD_BUTTON_RIGHT_FACE_DOWN))nav.a=true;
        if(IsGamepadButtonPressed(gp,GAMEPAD_BUTTON_RIGHT_FACE_LEFT))nav.y=true;
        if(IsGamepadButtonPressed(gp,GAMEPAD_BUTTON_MIDDLE_RIGHT))nav.start=true;
        if(IsGamepadButtonPressed(gp,GAMEPAD_BUTTON_LEFT_TRIGGER_1))nav.lb=true;
        if(IsGamepadButtonPressed(gp,GAMEPAD_BUTTON_RIGHT_TRIGGER_1))nav.rb=true;
        if(GetGamepadAxisMovement(gp,GAMEPAD_AXIS_LEFT_Y)<-0.6f)nav.up=true;
        if(GetGamepadAxisMovement(gp,GAMEPAD_AXIS_LEFT_Y)>0.6f)nav.down=true;
        if(GetGamepadAxisMovement(gp,GAMEPAD_AXIS_LEFT_X)<-0.6f)nav.left=true;
        if(GetGamepadAxisMovement(gp,GAMEPAD_AXIS_LEFT_X)>0.6f)nav.right=true;
    }
    if(IsKeyPressed(KEY_LEFT))nav.left=true;
    if(IsKeyPressed(KEY_RIGHT))nav.right=true;
    if(IsKeyPressed(KEY_UP))nav.up=true;
    if(IsKeyPressed(KEY_DOWN))nav.down=true;
    if(IsKeyPressed(KEY_ENTER)||IsKeyPressed(KEY_SPACE))nav.a=true;
    if(IsKeyPressed(KEY_ESCAPE))nav.b=true;
    if(IsKeyPressed(KEY_Y))nav.y=true;
    if(IsKeyPressed(KEY_PAGE_UP))nav.lb=true;
    if(IsKeyPressed(KEY_PAGE_DOWN))nav.rb=true;
}

/* ---- real pads only ------------------------------------------------------ */
// GLFW's mapping check filters out wireless mouse dongles and other
// joystick-shaped noise; what remains is what the picker offers.
static std::vector<std::string> mapped_pads(){
    std::vector<std::string> out;
    for(int i=0;i<16;i++)
        if(IsGamepadAvailable(i)&&glfwJoystickIsGamepad(i)){
            const char *n=GetGamepadName(i);
            out.push_back(n&&*n?n:"Unknown pad");
        }
    return out;
}
static std::string cycle_pad(const std::string &cur,int dir){
    std::vector<std::string> pads={""}; // "" = automatic: first mapped pad
    const auto real=mapped_pads();
    pads.insert(pads.end(),real.begin(),real.end());
    auto it=std::find(pads.begin(),pads.end(),cur);
    const size_t at=it==pads.end()?0:(size_t)(it-pads.begin());
    return pads[(at+(dir>0?1:pads.size()-1))%pads.size()];
}

/* ---- save data: symlink-safe helpers ------------------------------------- */
// Refuse a path that is, or passes through, a symbolic link.
static void refuse_symlinks(const fs::path &p,const fs::path &root){
    for(fs::path q=p;!q.empty();q=q.parent_path()){
        if(fs::is_symlink(q))throw std::runtime_error("Save path contains a symbolic link");
        if(q==root||!q.has_parent_path())break;
    }
}

// The newest game log in the launcher's log dir, for the About block.
static std::string latest_log(const fs::path &data,const std::string &id){
    std::error_code ec;
    fs::path best;fs::file_time_type best_time{};
    for(const auto &e:fs::directory_iterator(data/"logs",ec)){
        const auto n=e.path().filename().string();
        if(n.rfind(id+"-",0)!=0||n.size()<5||n.compare(n.size()-4,4,".log")!=0){ec.clear();continue;}
        const auto t=fs::last_write_time(e,ec);
        if(ec){ec.clear();continue;}
        if(best.empty()||t>best_time){best=e.path();best_time=t;}
    }
    return best.empty()?"":best.string();
}

/* ---- rows ---------------------------------------------------------------- */
static bool hit(Rectangle r){return CheckCollisionPointRec(GetMousePosition(),r);}
// A menu row in the rexmenu style; returns true on click. dim greys it out.
static bool row_draw(Rectangle r,const std::string &label,bool sel,bool dim=false){
    DrawRectangleRounded(r,0.18f,8,sel?col_sel:col_panel);
    if(sel)DrawRectangleRoundedLinesEx(r,0.18f,8,2.0f,col_outline);
    ui_text(label,(int)r.x+18,(int)r.y+((int)r.height-24)/2,24,
            dim?Color{150,150,150,255}:RAYWHITE);
    return !dim&&hit(r)&&IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
}
// A settings row with a "< value >" cycler on the right side; the value is
// shrunk from the left so a long pad name never collides with the label.
static void row_value(Rectangle r,const std::string &label,const std::string &value,bool sel,bool dim=false){
    DrawRectangleRounded(r,0.18f,8,sel?col_sel:col_panel);
    if(sel)DrawRectangleRoundedLinesEx(r,0.18f,8,2.0f,col_outline);
    const Color text=dim?Color{150,150,150,255}:RAYWHITE;
    ui_text(label,(int)r.x+18,(int)r.y+((int)r.height-24)/2,24,text);
    const float label_w=ui_measure(label,24);
    const std::string v="< "+fit_left(value,24,r.width-36-label_w-30)+" >";
    ui_text(v,(int)(r.x+r.width-18-ui_measure(v,24)),(int)r.y+((int)r.height-24)/2,24,text);
}

enum class Screen { Home, Settings, Files, About };

// All the per-frame UI state in one place. Held by reference through the
// main loop; the launcher is single-threaded so passing it around is cheap.
struct State {
    int selected=0;
    Screen screen=Screen::Home;
    Nav nav{};
    int home_sel=0, settings_sel=0, files_sel=0;
    int files_game=0; // which game the Files-screen actions target
    // Rail hover state, one frame stale by design (see the draw below).
    int rail_hover=-1;
    float rail_scroll_x=0.0f;
    bool rail_scroll_ready=false;
    // GDI import: one async zenity .gdi picker at a time (single-file
    // selection), then a forked tools/import_gdi.py child PER GAME. The
    // Game files screen lets you queue imports for several games while
    // earlier ones are still on disk.
    pid_t gdi_pid=0;
    int gdi_fd=-1, gdi_game=0;
    std::string gdi_text;
    std::vector<pid_t> import_pid;
    std::vector<std::string> import_log;
    bool confirming_clear=false;
    int clear_game=0;
};

int main(int argc,char **argv){
    try {
    const fs::path home=getenv("HOME")?getenv("HOME"):"";
    if(home.empty()){fprintf(stderr,"HOME is unset\n");return 1;}
    const fs::path data=base_dir("XDG_DATA_HOME",".local/share")/"recomp-launcher";
    const fs::path cfg=base_dir("XDG_CONFIG_HOME",".config")/"recomp-launcher";
    fs::create_directories(cfg);
    // One launcher at a time.
    int lock=open((cfg/"launcher.lock").c_str(),O_CREAT|O_RDWR|O_CLOEXEC,0600);
    if(lock<0||flock(lock,LOCK_EX|LOCK_NB)){fprintf(stderr,"Launcher is already open or config is not writable\n");return 1;}

    // Build the rail from the candidates whose launch.sh is on disk. A
    // game without a launch.sh is dropped entirely (no "missing launcher"
    // surprise when the user presses Play). max_fps caps the global
    // FPS setting at launch; MSR is locked to its native 30Hz.
    std::vector<Game> games;
    {
        const std::vector<Game> all={
            Game{"powerstone","Power Stone","CAPCOM / 1999","powerstone-native","powerstone_host",
                 "SH4 binary recompiled to C, built as","native x86-64 - no emulator anywhere.",120},
            Game{"powerstone2","Power Stone 2","CAPCOM / 2000","powerstone2-native","powerstone2_host",
                 "Same 100% native SH4-to-C treatment as","Power Stone. Import its .gdi to install.",120},
            Game{"msr","Metropolis Street Racer","BIZARRE CREATIONS / 2000","msr-native","msr_host",
                 "Bizarre's SH4 code recompiled to native C;","renderer mapped to raylib/OpenGL, no emulator.",30}};
        std::error_code ec;
        for(const auto &g:all){
            if(fs::exists(home/g.dir/"launch.sh",ec))games.push_back(g);
            ec.clear();
        }
    }
    // Settings are global (one per launcher, not one per game). Save data
    // is per game and gets stamped into Settings.saves at launch() time.
    Settings gs;
    std::string status="Choose a game.";
    bool status_err=false;
    try{gs=load_settings(cfg/"launcher.conf",gs);}
    catch(const std::exception &e){status=e.what();status_err=true;}
    // Make sure gs.saves is an absolute path before cycle_settings is ever
    // asked to auto-save; the value persisted in launcher.conf is just a
    // placeholder - launch() overwrites it with the per-game folder.
    if(!fs::path(gs.saves).is_absolute())
        gs.saves=(data/"saves"/games[0].id).string();
    for(const auto &g:games){
        std::error_code ec;
        fs::create_directories(data/"saves"/g.id,ec);
    }
    gs.mode=fixed_mode(gs.mode); // legacy virtual pad -> external
    auto set_status=[&](const std::string &s,bool err=false){status=s;status_err=err;};

    double smoke=0;int auto_game=-1;
    for(int i=1;i<argc;i++){
        if(!strcmp(argv[i],"--smoke")&&i+1<argc)smoke=atof(argv[++i]);
        else if(!strcmp(argv[i],"--launch")&&i+1<argc){
            const std::string id=argv[++i];
            for(int j=0;j<(int)games.size();j++)if(id==games[j].id)auto_game=j;
        }
    }

    SetConfigFlags(FLAG_MSAA_4X_HINT|FLAG_VSYNC_HINT);
    InitWindow(1120,790,"Retro Recomp");
    SetExitKey(KEY_NULL);   // Escape is B's job.
    SetTargetFPS(60);
    load_ui_font();

    // Cover art: assets/<id>.png next to the launcher, then the game dir
    // (icon.png, assets/*.png, any *.png near the root), else the letter tile.
    std::vector<ScopedTexture> tiles(games.size());
    for(int i=0;i<(int)games.size();i++){
        const fs::path root=home/games[i].dir;
        const fs::path asset=home/"recomp-launcher/assets"/(std::string(games[i].id)+".png");
        std::error_code ec;
        auto try_load=[&](const fs::path &p)->bool{
            if(!fs::exists(p,ec))return false;
            ScopedTexture t(p.string());
            if(!t.ok())return false;
            tiles[i]=std::move(t);
            return true;
        };
        if(try_load(asset))continue;
        if(try_load(root/"icon.png"))continue;
        bool done=false;
        for(const fs::path dir:{root/"assets",root}){
            std::vector<fs::path> pngs;
            for(const auto &e:fs::directory_iterator(dir,ec)){
                const auto n=e.path().filename().string();
                if(n.size()>4&&n.compare(n.size()-4,4,".png")==0)pngs.push_back(e.path());
                ec.clear();
            }
            std::sort(pngs.begin(),pngs.end());
            for(const auto &p:pngs)if(try_load(p)){done=true;break;}
            if(done)break;
        }
    }

    State st;
    st.import_pid.assign(games.size(),(pid_t)0);
    st.import_log.assign(games.size(),std::string{});
    const char *const kHomeItems[]={"Play","Game files","Settings","About","Quit"};
    const int kHomeCount=5;
    const char *const kSettingsItems[]={"Frame-rate limit","FPS overlay","Renderer",
                                       "Controller","External pad device"};
    const int kSettingsCount=5;
    const float kRowW=560.0f;   // settings rows; the GLOBAL heading sits right of them

    // Fork tools/import_gdi.py for game gi on the picked .gdi path; output
    // goes to a per-import log so a failure is diagnosable after the fact.
    auto start_import=[&](int gi,const std::string &gdi_path){
        const Game &ig=games[gi];
        std::error_code ec;
        const fs::path self=fs::canonical("/proc/self/exe",ec);
        fs::path tool;
        if(!ec&&fs::exists(self.parent_path()/"tools/import_gdi.py"))
            tool=self.parent_path()/"tools/import_gdi.py";
        else tool=home/"recomp-launcher/tools/import_gdi.py";
        if(!fs::exists(tool))throw std::runtime_error("Import tool not found: "+tool.string());
        fs::create_directories(data/"logs");
        st.import_log[gi]=(data/"logs"/(std::string(ig.id)+"-import-"+std::to_string(time(nullptr))+".log")).string();
        int fd=open(st.import_log[gi].c_str(),O_WRONLY|O_CREAT|O_APPEND,0600);
        if(fd<0)throw std::runtime_error("Cannot create import log");
        pid_t pid=fork();
        if(pid==0){
            dup2(fd,STDOUT_FILENO);dup2(fd,STDERR_FILENO);close(fd);
            const std::string dest=(home/ig.dir/"disc").string();
            execlp("python3","python3",tool.c_str(),gdi_path.c_str(),dest.c_str(),(char*)nullptr);
            perror("exec import");_exit(127);
        }
        close(fd);if(pid<0)throw std::runtime_error("Cannot start import");
        st.import_pid[gi]=pid;
    };
    // One cycler for the global settings rows, shared by pad, keyboard and
    // mouse. Every mutation auto-saves: leaving the menu with a half-saved
    // renderer choice is worse than one extra fsync.
    auto cycle_settings=[&](int row,int dir){
        const int max_fps=games[st.selected].max_fps;
        switch(row){
            case 0: gs.fps=cycle_fps(gs.fps,dir,max_fps);break;
            case 1: gs.show=!gs.show;break;
            case 2: gs.renderer=gs.renderer?0:1;break;
            case 3: gs.mode=gs.mode==2?0:2;break;
            case 4: if(gs.mode==0)gs.pad=cycle_pad(gs.pad,dir);break;
        }
        try{
            save_settings(cfg/"launcher.conf",gs);
            set_status("Settings saved");
        }catch(const std::exception &e){set_status(e.what(),true);}
    };
    // Files rows: one row per game (status) + 3 action rows that target
    // files_game (the LB/RB-selected game). Import GDI is non-blocking and
    // per-game, so the user can pick a .gdi for game A, navigate to game B,
    // and pick another .gdi while A's import is still on disk.
    auto start_picker_for=[&](int gi){
        if(st.gdi_fd>=0)return;
        const Game &g=games[gi];
        if(g.pid)return;
        int pipes[2];
        if(pipe(pipes)!=0)return;
        st.gdi_pid=fork();
        if(st.gdi_pid==0){
            dup2(pipes[1],STDOUT_FILENO);close(pipes[0]);close(pipes[1]);
            execlp("zenity","zenity","--file-selection",
                   "--title=Choose a Dreamcast .gdi dump (or a zip holding one)",
                   "--file-filter=Dreamcast GDI | *.gdi *.GDI *.zip *.ZIP",(char*)nullptr);
            _exit(127);
        }
        close(pipes[1]);
        if(st.gdi_pid>0){
            st.gdi_fd=pipes[0];fcntl(st.gdi_fd,F_SETFL,O_NONBLOCK);
            st.gdi_text.clear();st.gdi_game=gi;
        }else close(pipes[0]);
    };
    auto request_clear_for=[&](int gi){
        if(games[gi].pid||st.import_pid[gi]||st.gdi_fd>=0)return;
        st.confirming_clear=true;st.clear_game=gi;
    };

    auto launch=[&](Game &g,const Settings &s){
        fs::path root=home/g.dir;
        if(access((root/"launch.sh").c_str(),X_OK))throw std::runtime_error("Game launcher not found: "+root.string());
        Settings sforlaunch=s;
        sforlaunch.saves=(data/"saves"/g.id).string();
        // Defensive clamp: gs.fps might have been cycled up on a previous
        // game (Power Stone can hit 120) before the user picked MSR, which
        // is locked to its native 30. The same value is sent to the game
        // hosts via MSR_FPS / RECOMP_FPS below.
        const int eff_fps=std::min(s.fps,g.max_fps);
        auto vmu=prepare_save(sforlaunch,root/"saves/vmu_a1.bin");
        fs::create_directories(data/"logs");
        g.log=(data/"logs"/(std::string(g.id)+"-"+std::to_string(time(nullptr))+".log")).string();
        int fd=open(g.log.c_str(),O_WRONLY|O_CREAT|O_APPEND,0600);
        if(fd<0)throw std::runtime_error("Cannot create game log");
        pid_t pid=fork();
        if(pid==0){
            dup2(fd,STDOUT_FILENO);dup2(fd,STDERR_FILENO);close(fd);
            for(char **e=environ;*e;){
                std::string key=*e;key=key.substr(0,key.find('='));
                if(key.rfind("MSR_",0)==0||key.rfind("PS_",0)==0||key.rfind("RECOMP_",0)==0){unsetenv(key.c_str());e=environ;}else ++e;
            }
            // MSR's in-game clock wants UTC - the recomp reads it via the
            // libc localtime path, and the host system's BST was being read
            // as +8 hours from the real time. Europe/London is Bizarre's
            // origin, but UTC keeps every location in MSR on the right
            // wall-clock minute regardless of the host's zone.
            if(std::strcmp(g.id,"msr")==0)setenv("TZ","UTC",1);
            const char *rend=s.renderer==1?"vulkan":"raylib";
            setenv("RECOMP_RENDERER",rend,1);setenv("POWERSTONE_RENDERER",rend,1);
            setenv("MSR_VULKAN",s.renderer==1?"1":"0",1);
            setenv("MSR_VMU",vmu.c_str(),1);setenv("MSR_FPS",std::to_string(eff_fps).c_str(),1);
            setenv("RECOMP_FPS",std::to_string(eff_fps).c_str(),1);
            setenv("RECOMP_PAD",mode_env(s.mode),1);
            setenv("RECOMP_GAMEPAD",s.pad.c_str(),1);setenv("RECOMP_SHOW_FPS",s.show?"1":"0",1);
            if(chdir(root.c_str())){perror("chdir");_exit(126);}
            execl("./launch.sh","./launch.sh",(char*)nullptr);perror("exec launch");_exit(127);
        }
        close(fd);if(pid<0)throw std::runtime_error("Cannot start game");g.pid=pid;
    };

    if(auto_game>=0){
        st.selected=auto_game;
        Game &g=games[st.selected];
        std::error_code ec;
        const bool has=fs::exists(home/g.dir/"disc/1ST_READ.BIN",ec);
        if(!has)set_status(std::string(g.title)+" has no game content yet - import a .gdi first (Game files).",true);
        else try{launch(g,gs);set_status("Running "+std::string(g.title));}
        catch(const std::exception &e){set_status(e.what(),true);}
    }

    auto switch_game=[&](int idx){
        if(idx<0||idx>=(int)games.size())return;
        st.selected=idx;st.home_sel=0;st.settings_sel=0;st.files_sel=0;
        st.confirming_clear=false;
    };
    auto any_running=[&]{
        for(const auto &gg:games)if(gg.pid)return true;
        return false;
    };
    auto has_content=[&](const Game &g){
        std::error_code ec;
        const fs::path root=home/g.dir;
        return fs::exists(root/"disc/1ST_READ.BIN",ec)
            && fs::exists(root/"disc/gdmap.txt",ec);
    };
    auto try_play=[&]{
        Game &g=games[st.selected];
        if(any_running()){
            set_status("A game is already running - close it before starting another.",true);
            return;
        }
        if(!has_content(g)){
            set_status(std::string(g.title)+" has no game content yet - import a .gdi first (Game files).",true);
            return;
        }
        try{
            launch(g,gs);
            set_status("Running "+std::string(g.title)+". Saves: "+(data/"saves"/g.id).string());
        }catch(const std::exception &e){set_status(e.what(),true);}
    };

    while(!WindowShouldClose()){
        if(smoke>0&&GetTime()>smoke)break;
        poll_input(st.nav);
        const int w=GetScreenWidth(),h=GetScreenHeight();
        Game &g=games[st.selected];

        // Reap finished games and report how they went.
        for(auto &gg:games)if(gg.pid){
            int code;pid_t p=waitpid(gg.pid,&code,WNOHANG);
            if(p==gg.pid){
                gg.pid=0;
                set_status(std::string(gg.title)+(WIFEXITED(code)&&WEXITSTATUS(code)==0
                    ?" closed.":" exited; see game log: "+gg.log));
            }
        }
        // GDI picker finished: kick off the import child for that game.
        if(st.gdi_fd>=0){
            char buf[1024];ssize_t n;
            while((n=read(st.gdi_fd,buf,sizeof buf))>0)st.gdi_text.append(buf,n);
            if(n==0){
                int code;waitpid(st.gdi_pid,&code,0);close(st.gdi_fd);st.gdi_fd=-1;
                if(WIFEXITED(code)&&WEXITSTATUS(code)==0){
                    while(!st.gdi_text.empty()&&(st.gdi_text.back()=='\n'||st.gdi_text.back()=='\r'))st.gdi_text.pop_back();
                    if(!st.gdi_text.empty()){
                        try{
                            start_import(st.gdi_game,st.gdi_text);
                            set_status("Importing "+std::string(games[st.gdi_game].title)+
                                       " content from the .gdi - this can take a minute...");
                        }catch(const std::exception &e){set_status(e.what(),true);}
                    }
                }
            }
        }
        // Reap a finished import per game. Multiple imports can run in parallel
        // (one per game), so we walk all slots each frame.
        for(int i=0;i<(int)st.import_pid.size();++i){
            if(!st.import_pid[i])continue;
            int code;pid_t p=waitpid(st.import_pid[i],&code,WNOHANG);
            if(p==st.import_pid[i]){
                const bool ok=WIFEXITED(code)&&WEXITSTATUS(code)==0;
                st.import_pid[i]=0;
                set_status(ok?std::string(games[i].title)+" content imported - ready to play."
                             :"Import failed; see log: "+st.import_log[i],!ok);
            }
        }

        // LB/RB switches the active game from any non-modal screen. The
        // confirmation dialog owns the keys; import is per-game and stays
        // running if the user pages through the rail. On the Files screen
        // the LB/RB target is the action cursor (files_game) rather than
        // the rail selection, so the user can pivot between games without
        // changing the Home rail.
        const bool allow_switch=!st.confirming_clear;
        if(allow_switch&&st.nav.lb!=st.nav.rb){
            const int dir=st.nav.rb?1:(int)games.size()-1;
            if(st.screen==Screen::Files){
                st.files_game=(st.files_game+dir)%(int)games.size();
                st.files_sel=st.files_game; // jump cursor onto the game row
            }else switch_game((st.selected+dir)%(int)games.size());
        }

        if(st.screen==Screen::Home){
            if(st.nav.up)st.home_sel=(st.home_sel+kHomeCount-1)%kHomeCount;
            if(st.nav.down)st.home_sel=(st.home_sel+1)%kHomeCount;
            const float wheel=GetMouseWheelMove();
            if(wheel!=0.0f)switch_game((st.selected+(wheel<0.0f?1:(int)games.size()-1))%(int)games.size());
            // Clicking an icon: first click selects, second click switches.
            if(st.rail_hover>=0&&IsMouseButtonPressed(MOUSE_BUTTON_LEFT)){
                if(st.selected==st.rail_hover)switch_game(st.rail_hover);
                else st.selected=st.rail_hover;
            }
            if(st.nav.left||st.nav.right)
                switch_game((st.selected+(st.nav.right?1:(int)games.size()-1))%(int)games.size());
            if(st.nav.a||st.nav.start){
                switch(st.home_sel){
                    case 0: try_play();break;
                    case 1: st.screen=Screen::Files;st.files_sel=0;break;
                    case 2: st.screen=Screen::Settings;st.settings_sel=0;break;
                    case 3: st.screen=Screen::About;break;
                    case 4: goto done;
                }
            }
        }else if(st.screen==Screen::Files){
            // Layout: [game 0..N-1, Import GDI, Clear data, Back]. The
            // cursor moves through all of them with UP/DOWN; A on a game
            // row sets files_game and jumps to the Import row, A on an
            // action row fires for files_game. LB/RB are a shortcut for
            // moving among game rows only.
            const int kActionCount=3;
            const int total_rows=(int)games.size()+kActionCount;
            const int import_row=(int)games.size();
            const int clear_row=(int)games.size()+1;
            const int back_row=(int)games.size()+2;
            if(st.confirming_clear){
                if(st.nav.b||st.nav.y)st.confirming_clear=false;
                else if(st.nav.a){
                    Game &cg=games[st.clear_game];
                    if(cg.pid||st.import_pid[st.clear_game]){
                        set_status("Cannot clear while the game or an import is running.",true);
                    }else try{
                        const fs::path disc=home/cg.dir/"disc";
                        if(fs::is_symlink(disc))throw std::runtime_error("Disc folder is a symbolic link");
                        std::error_code ec;
                        if(fs::exists(disc,ec)){
                            for(const auto &e:fs::directory_iterator(disc)){
                                refuse_symlinks(e.path(),disc);
                                fs::remove_all(e.path());
                            }
                        }else fs::create_directories(disc);
                        set_status("Cleared "+std::string(cg.title)+" data - import a .gdi to reinstall the content.");
                    }catch(const std::exception &e){set_status(std::string("Clear failed: ")+e.what(),true);}
                    st.confirming_clear=false;
                }
                st.nav.consume();
            }else{
                if(st.nav.up)st.files_sel=(st.files_sel+total_rows-1)%total_rows;
                if(st.nav.down)st.files_sel=(st.files_sel+1)%total_rows;
                if(st.nav.a){
                    if(st.files_sel<import_row){
                        st.files_game=st.files_sel;st.files_sel=import_row;
                    }else if(st.files_sel==import_row)start_picker_for(st.files_game);
                    else if(st.files_sel==clear_row)request_clear_for(st.files_game);
                    else if(st.files_sel==back_row)st.screen=Screen::Home;
                }
                if(st.nav.b)st.screen=Screen::Home;
            }
        }else if(st.screen==Screen::Settings){
            if(st.nav.up)st.settings_sel=(st.settings_sel+kSettingsCount-1)%kSettingsCount;
            if(st.nav.down)st.settings_sel=(st.settings_sel+1)%kSettingsCount;
            const int cycle=st.nav.right?1:(st.nav.left?-1:0);
            if(cycle)cycle_settings(st.settings_sel,cycle);
            if(st.nav.a)cycle_settings(st.settings_sel,1);
            if(st.nav.b)st.screen=Screen::Home;
        }else if(st.screen==Screen::About){
            if(st.nav.b)st.screen=Screen::Home;
        }

        /* ---- draw ------------------------------------------------------- */
        BeginDrawing();
        ClearBackground(col_bg);
        const int margin=36;

        if(st.screen==Screen::Home){
            ui_text("RETRO RECOMP",margin,margin,44,RAYWHITE);
            ui_text("Dreamcast native recomp library",margin,margin+52,16,col_muted);

            // The game rail: one dashboard tile per game, laid out from the
            // centre, eased so the selected tile stays centred.
            const float cell=179.0f,icon=141.0f;
            const float rail_w=cell*(float)games.size();
            const float target_rail_x=rail_w<=w-72?(w-rail_w)/2.0f:
                std::clamp(w/2.0f-cell*(st.selected+0.5f),w-36-rail_w,36.0f);
            if(!st.rail_scroll_ready){st.rail_scroll_x=target_rail_x;st.rail_scroll_ready=true;}
            else{
                const float easing=std::min(1.0f,GetFrameTime()*9.0f);
                st.rail_scroll_x+=(target_rail_x-st.rail_scroll_x)*easing;
                if(std::abs(target_rail_x-st.rail_scroll_x)<0.25f)st.rail_scroll_x=target_rail_x;
            }
            const float rail_x=st.rail_scroll_x;
            const float rail_y=(float)margin+96;
            st.rail_hover=-1;
            BeginScissorMode(margin,(int)rail_y-10,w-2*margin,(int)icon+70);
            for(int i=0;i<(int)games.size();++i){
                const float cx=rail_x+cell*(float)i+cell/2.0f;
                const float cy=rail_y+icon/2.0f+14.0f;
                const bool sel=(i==st.selected);
                const Rectangle cellrec{cx-cell/2.0f+6,rail_y,cell-12,icon+28};
                if(CheckCollisionPointRec(GetMousePosition(),cellrec))st.rail_hover=i;
                // A gentle lift for the loaded game, a stronger one for the
                // icon under the finger.
                float lift=sel?-4.0f:0.0f;
                if(i==st.rail_hover)lift=-8.0f;
                const float size=(sel||i==st.rail_hover)?icon+10.0f:icon;
                const Rectangle ic{cx-size/2.0f,cy-size/2.0f+lift,size,size};
                if(i==st.rail_hover)DrawRectangleRounded(cellrec,0.18f,8,Color{40,110,120,120});
                else if(sel)DrawRectangleRounded(cellrec,0.18f,8,Color{255,255,255,18});
                if(tiles[i].ok()){
                    DrawTexturePro(tiles[i].tex(),
                        {0,0,(float)tiles[i].tex().width,(float)tiles[i].tex().height},
                        ic,{0,0},0.0f,sel||i==st.rail_hover?WHITE:Color{200,205,212,255});
                }else{
                    DrawRectangleRounded(ic,0.14f,8,col_tile);
                    const std::string initial=games[i].title[0]?std::string(1,games[i].title[0]):"?";
                    ui_text_center(initial,(int)cx,(int)(cy-16+lift),32,col_letter);
                }
                ui_text_center(games[i].title,(int)cx,(int)(rail_y+icon+20),15,col_letter);
                if(games[i].pid)
                    ui_text_center("RUNNING",(int)cx,(int)(rail_y+icon+38),13,col_ok);
            }
            EndScissorMode();

            // Actions under the rail.
            const bool content=has_content(g);
            const int item_h=48;
            const int list_y=(int)rail_y+188;
            for(int i=0;i<kHomeCount;++i){
                const Rectangle row{(float)margin,(float)(list_y+i*item_h),420.0f,(float)(item_h-10)};
                const bool sel=i==st.home_sel;
                const bool running=any_running();
                const bool dim=(i==0&&(running||!content));
                std::string label=kHomeItems[i];
                if(i==0)label=running?"RUNNING - close the game to return"
                                 :(content?"Play":"Play (import a .gdi first - Game files)");
                else if(i==1)label=content?"Game files (content imported)":"Game files (no content)";
                if(row_draw(row,label,sel,dim)){
                    if(i==0)try_play();
                    else if(i==1){st.screen=Screen::Files;st.files_sel=0;}
                    else if(i==2){st.screen=Screen::Settings;st.settings_sel=0;}
                    else if(i==3){st.screen=Screen::About;}
                    else if(i==4){EndDrawing();goto done;}
                }
            }

            // Game info + global settings, right of the rows.
            const int info_x=margin+470;
            if(tiles[st.selected].ok()){
                const auto t=tiles[st.selected].tex();
                DrawTexturePro(t,{0,0,(float)t.width,(float)t.height},
                               {(float)info_x,(float)list_y,64,64},{0,0},0,WHITE);
            }
            ui_text(g.title,info_x+(tiles[st.selected].ok()?82:0),list_y,28,RAYWHITE);
            ui_text(g.tag,info_x+(tiles[st.selected].ok()?82:0),list_y+37,17,col_muted);
            ui_text(g.pid?"Running now":(content?"Ready to play":"No game data imported - import a .gdi in Game files"),
                    info_x,list_y+84,18,g.pid?col_ok:(content?col_warn:col_err));
            ui_text("FPS cap: "+std::to_string(gs.fps)+"   overlay: "+(gs.show?"on":"off")+
                    "   renderer: "+(gs.renderer?"Vulkan":"OpenGL"),
                    info_x,list_y+116,16,col_muted);
            ui_text(std::string("Controller: ")+mode_label(gs.mode),info_x,list_y+140,16,col_muted);

            ui_text(status,margin,h-72,18,status_err?col_err:col_ok);
            ui_text("F3 toggles the in-game FPS counter. Close the game with Escape.",
                    margin,h-46,15,col_hint);
            ui_text("A select   B back   LB/RB game   mouse: click / wheel",w-480,h-30,15,col_hint);
        }else if(st.screen==Screen::Files){
            ui_text("GAME FILES",margin,margin,36,RAYWHITE);
            ui_text("Install or clear content for any game on the rail",margin,margin+44,16,col_muted);
            // Show which game the action rows target (the "active" game).
            const Game &fg=games[st.files_game];
            ui_text("Actions target: "+std::string(fg.title)+" - "+fg.tag,margin,margin+72,15,col_teal);

            const int item_h=44;
            const int list_y=margin+108;
            std::error_code ec;
            // Per-game status rows first.
            for(int i=0;i<(int)games.size();++i){
                const Rectangle row{(float)margin,(float)(list_y+i*item_h),560.0f,(float)(item_h-8)};
                const bool sel=i==st.files_sel;
                const Game &gg=games[i];
                const fs::path disc=home/gg.dir/"disc";
                const bool has_1st=fs::exists(disc/"1ST_READ.BIN",ec);
                const bool has_map=fs::exists(disc/"gdmap.txt",ec);
                const bool imported=has_1st&&has_map;
                std::string tag=gg.tag;
                std::string status_text;
                Color st_col;
                if(st.import_pid[i]){status_text="Importing...";st_col=col_teal;}
                else if(imported){status_text="Content imported";st_col=col_ok;}
                else if(has_1st){status_text="gdmap.txt missing - re-import";st_col=col_warn;}
                else if(gg.pid){status_text="Game running";st_col=col_ok;}
                else {status_text="No content";st_col=col_warn;}
                DrawRectangleRounded(row,0.18f,8,sel?col_sel:col_panel);
                if(sel)DrawRectangleRoundedLinesEx(row,0.18f,8,2.0f,col_outline);
                ui_text(gg.title,(int)row.x+18,(int)row.y+((int)row.height-22)/2,22,RAYWHITE);
                const float title_w=ui_measure(gg.title,22);
                ui_text(tag,(int)(row.x+24+title_w),(int)row.y+((int)row.height-14)/2,14,col_muted);
                ui_text(status_text,(int)(row.x+row.width-18-ui_measure(status_text,18)),(int)row.y+((int)row.height-18)/2,18,st_col);
                if(sel&&hit(row)&&IsMouseButtonPressed(MOUSE_BUTTON_LEFT)){
                    st.files_game=i;st.files_sel=i;
                }
            }
            // Three action rows below the game list.
            const char *const kActions[]={"Import GDI image...","Clear imported data","Back"};
            const int action_y=list_y+(int)games.size()*item_h+8;
            for(int i=0;i<3;++i){
                const int row_idx=(int)games.size()+i;
                const Rectangle row{(float)margin,(float)(action_y+i*item_h),560.0f,(float)(item_h-8)};
                const bool sel=row_idx==st.files_sel;
                const bool dim=(i==0)&&(st.gdi_fd>=0)
                    || (i==1)&&(fg.pid||st.import_pid[st.files_game]||st.gdi_fd>=0);
                if(row_draw(row,kActions[i],sel,dim)){
                    if(!dim){
                        if(i==0)start_picker_for(st.files_game);
                        else if(i==1)request_clear_for(st.files_game);
                        else st.screen=Screen::Home;
                    }
                }
                if(!dim&&hit(row)&&IsMouseButtonPressed(MOUSE_BUTTON_LEFT)){
                    if(st.files_sel==row_idx){
                        if(i==0)start_picker_for(st.files_game);
                        else if(i==1)request_clear_for(st.files_game);
                        else st.screen=Screen::Home;
                    }else st.files_sel=row_idx;
                }
            }

            ui_text(status,margin,h-72,18,status_err?col_err:col_ok);
            ui_text("A select   B back   LB/RB: target game",margin,h-28,15,col_hint);
        }else if(st.screen==Screen::Settings){
            ui_text("SETTINGS",margin,margin,36,RAYWHITE);
            ui_text("Shared by every game in the rail",margin,margin+44,16,col_muted);

            const auto real_pads=mapped_pads();
            const std::string pad_name=gs.pad.empty()
                ?"Automatic: "+(real_pads.empty()?"no mapped pad found":real_pads.front())
                :gs.pad;
            const std::string values[]={std::to_string(gs.fps)+" FPS",gs.show?"On":"Off",
                                        gs.renderer?"Vulkan (SDL3)":"OpenGL (raylib)",
                                        mode_label(gs.mode),pad_name};
            const int item_h=52;
            const int list_y=margin+100;
            for(int i=0;i<kSettingsCount;++i){
                const Rectangle row{(float)margin,(float)(list_y+i*item_h),kRowW,(float)(item_h-10)};
                const bool sel=i==st.settings_sel;
                const bool dim=(i==4&&gs.mode!=0);
                row_value(row,kSettingsItems[i],values[i],sel,dim);
                // Mouse: click selects; clicking a cycler changes it.
                if(!dim&&hit(row)&&IsMouseButtonPressed(MOUSE_BUTTON_LEFT)){
                    if(st.settings_sel==i)cycle_settings(i,1);
                    else st.settings_sel=i;
                }
            }
            int note_y=list_y+kSettingsCount*item_h+14;
            ui_text("Limits presentation only; 120 FPS does not unlock native game timing.",
                    margin,note_y,16,col_muted);
            ui_text(gs.mode==2?"Keyboard in game: Space/Z A, X B, C X, V Y, Enter Start, arrows D-pad, WASD stick, Q/E triggers."
                              :std::to_string(real_pads.size())+" real pad(s) mapped. Empty device means automatic.",
                    margin,note_y+26,16,col_muted);

            // A short heading on the right tells the user these are shared
            // - the rows themselves are value cyclers, nothing more.
            const int head_x=margin+(int)kRowW+120;
            ui_text("GLOBAL SETTINGS",head_x,list_y,17,col_teal);
            ui_text("One launcher.conf is shared by every game in the rail.",
                    head_x,list_y+24,14,col_muted);
            ui_text("Pad, FPS cap and renderer choices persist for whichever game you pick next.",
                    head_x,list_y+44,14,col_muted);

            ui_text(status,margin,h-72,18,status_err?col_err:col_ok);
            ui_text("Left/Right change   A select   B back",margin,h-28,15,col_hint);
        }else if(st.screen==Screen::About){
            // Full-width left margin layout: header strip, then a single
            // column of labelled detail lines.
            ui_text("ABOUT",margin,margin,36,RAYWHITE);
            const fs::path root=home/g.dir;
            std::error_code ec;
            int dy=margin+84;
            if(tiles[st.selected].ok()){
                const auto t=tiles[st.selected].tex();
                DrawTexturePro(t,{0,0,(float)t.width,(float)t.height},
                               {(float)margin,(float)dy,80,80},{0,0},0,WHITE);
            }
            const int text_x=margin+(tiles[st.selected].ok()?98:0);
            ui_text(g.title,text_x,dy,28,RAYWHITE);dy+=34;
            ui_text(g.tag,text_x,dy,16,col_muted);dy+=30;
            ui_text("100% NATIVE",text_x,dy,15,col_ok);dy+=22;
            ui_text(g.how1,text_x,dy,15,col_muted);dy+=21;
            ui_text(g.how2,text_x,dy,15,col_muted);dy+=36;
            const float det_w=(float)(w-margin-text_x);
            auto detail=[&](const std::string &label,const std::string &value,Color c){
                ui_text(label,text_x,dy,15,col_muted);
                ui_text(fit_left(value,15,det_w-ui_measure(label,15)-10),
                        text_x+(int)ui_measure(label,15)+10,dy,15,c);
                dy+=24;
            };
            detail("Game dir:",root.string(),col_muted);
            detail("Host binary:",std::string(g.host)+(fs::exists(root/g.host,ec)?" (found)":" (missing)"),
                   fs::exists(root/g.host)?col_ok:col_err);
            detail("Disc image:",has_content(g)?"disc/1ST_READ.BIN + gdmap.txt (found)":"disc/1ST_READ.BIN (missing)",
                   has_content(g)?col_ok:col_warn);
            detail("Renderer:",gs.renderer?"SDL3 / Vulkan":"raylib / OpenGL",col_muted);
            const std::string log=latest_log(data,g.id);
            detail("Latest log:",log.empty()?"none yet":log,col_muted);

            ui_text(status,margin,h-72,18,status_err?col_err:col_ok);
            ui_text("B back   LB/RB game",margin,h-28,15,col_hint);
        }

        // Clear-data confirmation, over everything.
        if(st.confirming_clear){
            const Game &cg=games[st.clear_game];
            DrawRectangle(0,0,w,h,Color{0,0,0,220});
            ui_text("Clear the imported data for "+std::string(cg.title)+"?",margin,h/2-50,28,RAYWHITE);
            ui_text("Removes everything in "+(home/cg.dir/"disc").string(),margin,h/2-6,17,col_muted);
            ui_text("Import a .gdi afterwards to reinstall the game content.",margin,h/2+20,17,col_muted);
            ui_text("A / Enter: confirm   B / Escape: cancel.",margin,h/2+54,20,
                    Color{200,205,212,255});
        }
        EndDrawing();
    }
done:
    CloseWindow();
    return 0;
    }catch(const std::exception &e){fprintf(stderr,"launcher: %s\n",e.what());return 1;}
}