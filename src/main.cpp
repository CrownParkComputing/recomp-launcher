// recomp-launcher - the library menu for the native Dreamcast recompiles,
// styled after rexmenu (the Xbox 360 family menu): same palette, same
// dashboard icon rail, same pad-first navigation.
//
//   recomp-launcher [--smoke <seconds>] [--launch <id>]
//
// Home is a game icon rail plus the text rows Play / Settings / Quit.
// Settings are per game (FPS cap, FPS overlay, renderer, controller mode,
// pad device) and persist through settings.hpp; the same page imports a
// game's content from a .gdi dump and clears it again. Play forks the
// game's own launch.sh with the recomp environment and logs its output;
// the menu waits for the child.
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
    const char *how1,*how2; // how the port runs, drawn in the details block
    Settings settings;
    pid_t pid=0;
    std::string log;
};

// Controller modes: 0 = external pad, 2 = keyboard. 1 was the virtual pad
// overlay, which the recomps no longer have - a legacy conf value of 1 is
// treated as 0 everywhere.
static const char *mode_label(int mode){return mode==2?"Keyboard":"External pad";}
static const char *mode_env(int mode){return mode==2?"keyboard":"external";}
static int fixed_mode(int mode){return mode==2?2:0;}

// FPS caps offered on the Settings row.
static int cycle_fps(int fps,int dir){
    const int rates[]={30,60,120};
    int at=1;for(int i=0;i<3;i++)if(fps==rates[i])at=i;
    return rates[(at+dir+3)%3];
}

// Play: save settings, then fork the game's launch.sh with the recomp
// environment. The child clears inherited MSR_/PS_/RECOMP_ diagnostics
// first, so a desktop launch never silently times out or hides.
static void launch(Game &g,const fs::path &home,const fs::path &data,const fs::path &cfg){
    save_settings(cfg/(std::string(g.id)+".conf"),g.settings);
    fs::path root=home/g.dir;
    if(access((root/"launch.sh").c_str(),X_OK))throw std::runtime_error("Game launcher not found: "+root.string());
    auto vmu=prepare_save(g.settings,root/"saves/vmu_a1.bin");
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
        const char *rend=g.settings.renderer==1?"vulkan":"raylib";
        setenv("RECOMP_RENDERER",rend,1);setenv("POWERSTONE_RENDERER",rend,1);
        setenv("MSR_VULKAN",g.settings.renderer==1?"1":"0",1);
        setenv("MSR_VMU",vmu.c_str(),1);setenv("MSR_FPS",std::to_string(g.settings.fps).c_str(),1);
        setenv("RECOMP_PAD",mode_env(g.settings.mode),1);
        setenv("RECOMP_GAMEPAD",g.settings.pad.c_str(),1);setenv("RECOMP_SHOW_FPS",g.settings.show?"1":"0",1);
        if(chdir(root.c_str())){perror("chdir");_exit(126);}
        execl("./launch.sh","./launch.sh",(char*)nullptr);perror("exec launch");_exit(127);
    }
    close(fd);if(pid<0)throw std::runtime_error("Cannot start game");g.pid=pid;
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

// The newest game log in the launcher's log dir, for the details block.
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

enum class Screen { Home, Settings };

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

    std::array<Game,3> games={
        Game{"powerstone","Power Stone","CAPCOM / 1999","powerstone-native","powerstone_host",
             "SH4 binary recompiled to C, built as","native x86-64 - no emulator anywhere.",
             Settings{},0,{}},
        Game{"powerstone2","Power Stone 2","CAPCOM / 2000","powerstone2-native","powerstone2_host",
             "Same 100% native SH4-to-C treatment as","Power Stone. Import its .gdi to install.",
             Settings{},0,{}},
        Game{"msr","Metropolis Street Racer","BIZARRE CREATIONS / 2000","msr-native","msr_host",
             "Bizarre's SH4 code recompiled to native C;","renderer mapped to raylib/OpenGL, no emulator.",
             Settings{},0,{}}};
    std::string status="Choose a game. Settings are saved per game.";
    bool status_err=false;
    for(auto &g:games){
        g.settings.saves=(data/"saves"/g.id).string();
        try{g.settings=load_settings(cfg/(std::string(g.id)+".conf"),g.settings);}
        catch(const std::exception &e){status=e.what();status_err=true;}
        g.settings.mode=fixed_mode(g.settings.mode); // legacy virtual pad -> external
    }
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
    std::array<ScopedTexture,3> tiles;
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

    int selected=0;             // active game
    Screen screen=Screen::Home;
    Nav nav;
    int home_sel=0,settings_sel=0;
    // Rail hover state, one frame stale by design (see the draw below).
    int rail_hover=-1;
    float rail_scroll_x=0.0f;
    bool rail_scroll_ready=false;
    // GDI import: async zenity .gdi picker, then a forked
    // tools/import_gdi.py child that fills the game's disc/ folder.
    pid_t gdi_pid=0;int gdi_fd=-1,gdi_game=0;std::string gdi_text;
    pid_t import_pid=0;int import_game=0;std::string import_log;
    bool confirming_clear=false;
    int clear_game=0;

    const char *const kHomeItems[]={"Play","Settings","Quit"};
    const int kHomeCount=3;
    const int kSettingsCount=8; // fps, overlay, renderer, mode, pad, import, clear, save
    const float kRowW=560.0f;   // settings rows; the details block sits right of them

    if(auto_game>=0){
        selected=auto_game;
        try{launch(games[selected],home,data,cfg);set_status("Running "+std::string(games[selected].title));}
        catch(const std::exception &e){set_status(e.what(),true);}
    }

    auto switch_game=[&](int idx){
        if(idx<0||idx>=(int)games.size())return;
        selected=idx;home_sel=0;settings_sel=0;
        confirming_clear=false;
    };
    auto any_running=[&]{
        for(const auto &gg:games)if(gg.pid)return true;
        return false;
    };
    auto try_play=[&]{
        Game &g=games[selected];
        if(any_running()){
            set_status("A game is already running - close it before starting another.",true);
            return;
        }
        std::error_code ec;
        if(!fs::exists(home/g.dir/"disc/1ST_READ.BIN",ec)){
            set_status(std::string(g.title)+" has no game content yet - import a .gdi first (Settings > Import GDI image).",true);
            return;
        }
        try{
            launch(g,home,data,cfg);
            set_status("Running "+std::string(g.title)+". Saves: "+g.settings.saves);
        }catch(const std::exception &e){set_status(e.what(),true);}
    };
    // Fork tools/import_gdi.py for game gi on the picked .gdi path; output
    // goes to a per-import log so a failure is diagnosable after the fact.
    auto start_import=[&](int gi,const std::string &gdi_path){
        Game &ig=games[gi];
        std::error_code ec;
        const fs::path self=fs::canonical("/proc/self/exe",ec);
        fs::path tool;
        if(!ec&&fs::exists(self.parent_path()/"tools/import_gdi.py"))
            tool=self.parent_path()/"tools/import_gdi.py";
        else tool=home/"recomp-launcher/tools/import_gdi.py";
        if(!fs::exists(tool))throw std::runtime_error("Import tool not found: "+tool.string());
        fs::create_directories(data/"logs");
        import_log=(data/"logs"/(std::string(ig.id)+"-import-"+std::to_string(time(nullptr))+".log")).string();
        int fd=open(import_log.c_str(),O_WRONLY|O_CREAT|O_APPEND,0600);
        if(fd<0)throw std::runtime_error("Cannot create import log");
        pid_t pid=fork();
        if(pid==0){
            dup2(fd,STDOUT_FILENO);dup2(fd,STDERR_FILENO);close(fd);
            const std::string dest=(home/ig.dir/"disc").string();
            execlp("python3","python3",tool.c_str(),gdi_path.c_str(),dest.c_str(),(char*)nullptr);
            perror("exec import");_exit(127);
        }
        close(fd);if(pid<0)throw std::runtime_error("Cannot start import");
        import_pid=pid;import_game=gi;
    };
    // One cycler for the settings rows, shared by pad, keyboard and mouse.
    auto cycle_settings=[&](int row,int dir){
        Game &g=games[selected];Settings &s=g.settings;
        switch(row){
            case 0: s.fps=cycle_fps(s.fps,dir);break;
            case 1: s.show=!s.show;break;
            case 2: s.renderer=s.renderer?0:1;break;
            case 3: s.mode=s.mode==2?0:2;break;
            case 4: if(s.mode==0)s.pad=cycle_pad(s.pad,dir);break;
            case 5: { // Import GDI image, via zenity (async, non-blocking)
                if(gdi_fd>=0||import_pid||g.pid)break;
                int pipes[2];
                if(pipe(pipes)==0){
                    gdi_pid=fork();
                    if(gdi_pid==0){
                        dup2(pipes[1],STDOUT_FILENO);close(pipes[0]);close(pipes[1]);
                        execlp("zenity","zenity","--file-selection",
                               "--title=Choose a Dreamcast .gdi dump (or a zip holding one)",
                               "--file-filter=Dreamcast GDI | *.gdi *.GDI *.zip *.ZIP",(char*)nullptr);
                        _exit(127);
                    }
                    close(pipes[1]);
                    if(gdi_pid>0){gdi_fd=pipes[0];fcntl(gdi_fd,F_SETFL,O_NONBLOCK);gdi_text.clear();gdi_game=selected;}
                    else close(pipes[0]);
                }
                break;
            }
            case 6: // Clear imported data (asks for confirmation)
                if(g.pid||import_pid||gdi_fd>=0)break;
                confirming_clear=true;clear_game=selected;
                break;
            case 7:
                try{
                    save_settings(cfg/(std::string(g.id)+".conf"),s);
                    set_status("Settings saved for "+std::string(g.title));
                }catch(const std::exception &e){set_status(e.what(),true);}
                break;
        }
    };

    while(!WindowShouldClose()){
        if(smoke>0&&GetTime()>smoke)break;
        poll_input(nav);
        const int w=GetScreenWidth(),h=GetScreenHeight();
        Game &g=games[selected];
        Settings &s=g.settings;

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
        if(gdi_fd>=0){
            char buf[1024];ssize_t n;
            while((n=read(gdi_fd,buf,sizeof buf))>0)gdi_text.append(buf,n);
            if(n==0){
                int code;waitpid(gdi_pid,&code,0);close(gdi_fd);gdi_fd=-1;
                if(WIFEXITED(code)&&WEXITSTATUS(code)==0){
                    while(!gdi_text.empty()&&(gdi_text.back()=='\n'||gdi_text.back()=='\r'))gdi_text.pop_back();
                    if(!gdi_text.empty()){
                        try{
                            start_import(gdi_game,gdi_text);
                            set_status("Importing "+std::string(games[gdi_game].title)+
                                       " content from the .gdi - this can take a minute...");
                        }catch(const std::exception &e){set_status(e.what(),true);}
                    }
                }
            }
        }
        // Reap a finished import and report.
        if(import_pid){
            int code;pid_t p=waitpid(import_pid,&code,WNOHANG);
            if(p==import_pid){
                const bool ok=WIFEXITED(code)&&WEXITSTATUS(code)==0;
                import_pid=0;
                set_status(ok?std::string(games[import_game].title)+" content imported - ready to play."
                             :"Import failed; see log: "+import_log,!ok);
            }
        }

        // LB/RB switches the active game from any screen (not mid-confirm:
        // the dialog owns the keys then).
        if(!confirming_clear&&nav.lb!=nav.rb)
            switch_game((selected+(nav.rb?1:(int)games.size()-1))%(int)games.size());

        if(screen==Screen::Home){
            if(nav.up)home_sel=(home_sel+kHomeCount-1)%kHomeCount;
            if(nav.down)home_sel=(home_sel+1)%kHomeCount;
            const float wheel=GetMouseWheelMove();
            if(wheel!=0.0f)switch_game((selected+(wheel<0.0f?1:(int)games.size()-1))%(int)games.size());
            // Clicking an icon: first click selects, second click switches.
            if(rail_hover>=0&&IsMouseButtonPressed(MOUSE_BUTTON_LEFT)){
                if(selected==rail_hover)switch_game(rail_hover);
                else selected=rail_hover;
            }
            if(nav.left||nav.right)
                switch_game((selected+(nav.right?1:(int)games.size()-1))%(int)games.size());
            if(nav.a||nav.start){
                switch(home_sel){
                    case 0: try_play();break;
                    case 1: screen=Screen::Settings;settings_sel=0;break;
                    case 2: goto done;
                }
            }
        }else if(screen==Screen::Settings){
            if(confirming_clear){
                if(nav.b||nav.y)confirming_clear=false;
                else if(nav.a){
                    Game &cg=games[clear_game];
                    if(cg.pid||import_pid){
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
                    confirming_clear=false;
                }
                nav.consume();
            }else{
                if(nav.up)settings_sel=(settings_sel+kSettingsCount-1)%kSettingsCount;
                if(nav.down)settings_sel=(settings_sel+1)%kSettingsCount;
                const int cycle=nav.right?1:(nav.left?-1:0);
                if(cycle)cycle_settings(settings_sel,cycle);
                if(nav.a)cycle_settings(settings_sel,1);
                if(nav.b)screen=Screen::Home;
            }
        }

        /* ---- draw ------------------------------------------------------- */
        BeginDrawing();
        ClearBackground(col_bg);
        const int margin=36;

        if(screen==Screen::Home){
            ui_text("RETRO RECOMP",margin,margin,44,RAYWHITE);
            ui_text("Dreamcast native recomp library",margin,margin+52,16,col_muted);

            // The game rail: one dashboard tile per game, laid out from the
            // centre, eased so the selected tile stays centred.
            const float cell=179.0f,icon=141.0f;
            const float rail_w=cell*(float)games.size();
            const float target_rail_x=rail_w<=w-72?(w-rail_w)/2.0f:
                std::clamp(w/2.0f-cell*(selected+0.5f),w-36-rail_w,36.0f);
            if(!rail_scroll_ready){rail_scroll_x=target_rail_x;rail_scroll_ready=true;}
            else{
                const float easing=std::min(1.0f,GetFrameTime()*9.0f);
                rail_scroll_x+=(target_rail_x-rail_scroll_x)*easing;
                if(std::abs(target_rail_x-rail_scroll_x)<0.25f)rail_scroll_x=target_rail_x;
            }
            const float rail_x=rail_scroll_x;
            const float rail_y=(float)margin+96;
            rail_hover=-1;
            BeginScissorMode(margin,(int)rail_y-10,w-2*margin,(int)icon+70);
            for(int i=0;i<(int)games.size();++i){
                const float cx=rail_x+cell*(float)i+cell/2.0f;
                const float cy=rail_y+icon/2.0f+14.0f;
                const bool sel=(i==selected);
                const Rectangle cellrec{cx-cell/2.0f+6,rail_y,cell-12,icon+28};
                if(CheckCollisionPointRec(GetMousePosition(),cellrec))rail_hover=i;
                // A gentle lift for the loaded game, a stronger one for the
                // icon under the finger.
                float lift=sel?-4.0f:0.0f;
                if(i==rail_hover)lift=-8.0f;
                const float size=(sel||i==rail_hover)?icon+10.0f:icon;
                const Rectangle ic{cx-size/2.0f,cy-size/2.0f+lift,size,size};
                if(i==rail_hover)DrawRectangleRounded(cellrec,0.18f,8,Color{40,110,120,120});
                else if(sel)DrawRectangleRounded(cellrec,0.18f,8,Color{255,255,255,18});
                if(tiles[i].ok()){
                    DrawTexturePro(tiles[i].tex(),
                        {0,0,(float)tiles[i].tex().width,(float)tiles[i].tex().height},
                        ic,{0,0},0.0f,sel||i==rail_hover?WHITE:Color{200,205,212,255});
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
            std::error_code ec;
            const bool has_content=fs::exists(home/g.dir/"disc/1ST_READ.BIN",ec);
            const int item_h=48;
            const int list_y=(int)rail_y+188;
            for(int i=0;i<kHomeCount;++i){
                const Rectangle row{(float)margin,(float)(list_y+i*item_h),420.0f,(float)(item_h-10)};
                const bool sel=i==home_sel;
                const bool running=any_running();
                const bool dim=(i==0&&(running||!has_content));
                const std::string label=i==0
                    ?(running?"RUNNING - close the game to return"
                             :(has_content?"Play":"Play (import a .gdi first - Settings)"))
                    :kHomeItems[i];
                if(row_draw(row,label,sel,dim)){
                    if(i==0)try_play();
                    else if(i==1){screen=Screen::Settings;settings_sel=0;}
                    else if(i==2){EndDrawing();goto done;}
                }
            }

            // About the active game, right of the rows.
            const int info_x=margin+470;
            if(tiles[selected].ok()){
                const auto t=tiles[selected].tex();
                DrawTexturePro(t,{0,0,(float)t.width,(float)t.height},
                               {(float)info_x,(float)list_y,64,64},{0,0},0,WHITE);
            }
            ui_text(g.title,info_x+(tiles[selected].ok()?82:0),list_y,28,RAYWHITE);
            ui_text(g.tag,info_x+(tiles[selected].ok()?82:0),list_y+37,17,col_muted);
            ui_text(g.pid?"Running now":(has_content?"Ready to play":"No game data imported - import a .gdi in Settings"),
                    info_x,list_y+84,18,g.pid?col_ok:(has_content?col_warn:col_err));
            ui_text("FPS cap: "+std::to_string(s.fps)+"   overlay: "+(s.show?"on":"off")+
                    "   renderer: "+(s.renderer?"Vulkan":"OpenGL"),
                    info_x,list_y+116,16,col_muted);
            ui_text(std::string("Controller: ")+mode_label(s.mode),info_x,list_y+140,16,col_muted);

            ui_text(status,margin,h-72,18,status_err?col_err:col_ok);
            ui_text("F3 toggles the in-game FPS counter. Close the game with Escape.",
                    margin,h-46,15,col_hint);
            ui_text("A select   B back   LB/RB game   mouse: click / wheel",w-480,h-30,15,col_hint);
        }else if(screen==Screen::Settings){
            ui_text(g.title,margin,margin,36,RAYWHITE);
            ui_text("Settings for this game",margin,margin+44,16,col_muted);

            const auto real_pads=mapped_pads();
            const std::string pad_name=s.pad.empty()
                ?"Automatic: "+(real_pads.empty()?"no mapped pad found":real_pads.front())
                :s.pad;
            const std::string labels[]={"Frame-rate limit","FPS overlay","Renderer","Controller",
                                        "External pad device","Import GDI image...","Clear imported data","Save settings"};
            const std::string values[]={std::to_string(s.fps)+" FPS",s.show?"On":"Off",
                                        s.renderer?"Vulkan (SDL3)":"OpenGL (raylib)",
                                        mode_label(s.mode),pad_name,"","",""};
            const int item_h=52;
            const int list_y=margin+100;
            for(int i=0;i<kSettingsCount;++i){
                const Rectangle row{(float)margin,(float)(list_y+i*item_h),kRowW,(float)(item_h-10)};
                const bool sel=i==settings_sel;
                const bool dim=(i==4&&s.mode!=0)||
                    ((i==5||i==6)&&(g.pid||import_pid||gdi_fd>=0));
                if(i>=5){
                    row_draw(row,labels[i],sel,dim);
                    const char *hint=i==5?"A pick a .gdi dump":i==6?"A clear (asks first)":"A save";
                    if(sel&&!dim){
                        const std::string h=hint;
                        ui_text(h,(int)(row.x+row.width-14-ui_measure(h,14)),(int)row.y+16,14,col_hint);
                    }
                }else{
                    row_value(row,labels[i],values[i],sel,dim);
                }
                // Mouse: click selects; clicking a cycler changes it.
                if(!dim&&hit(row)&&IsMouseButtonPressed(MOUSE_BUTTON_LEFT)){
                    if(settings_sel==i)cycle_settings(i,1);
                    else settings_sel=i;
                }
            }
            int note_y=list_y+kSettingsCount*item_h+14;
            ui_text("Limits presentation only; 120 FPS does not unlock native game timing.",margin,note_y,16,col_muted);
            ui_text(s.mode==2?"Keyboard in game: Space/Z A, X B, C X, V Y, Enter Start, arrows D-pad, WASD stick, Q/E triggers.":
                    TextFormat("%zu real pad(s) mapped. Empty device means automatic.",real_pads.size()),
                    margin,note_y+26,16,col_muted);

            // Recomp details for the selected game, right of the rows.
            const int det_x=margin+(int)kRowW+120;
            const float det_w=(float)(w-margin-det_x);
            const fs::path root=home/g.dir;
            std::error_code ec;
            int dy=list_y;
            ui_text("RECOMP DETAILS",det_x,dy,17,col_teal);dy+=30;
            ui_text(g.title,det_x,dy,20,RAYWHITE);dy+=26;
            ui_text(g.tag,det_x,dy,16,col_muted);dy+=28;
            ui_text("100% NATIVE",det_x,dy,15,col_ok);dy+=22;
            ui_text(g.how1,det_x,dy,15,col_muted);dy+=21;
            ui_text(g.how2,det_x,dy,15,col_muted);dy+=30;
            auto detail=[&](const std::string &label,const std::string &value,Color c){
                ui_text(label,det_x,dy,15,col_muted);
                ui_text(fit_left(value,15,det_w-ui_measure(label,15)-10),
                        det_x+(int)ui_measure(label,15)+10,dy,15,c);
                dy+=24;
            };
            detail("Game dir:",root.string(),col_muted);
            detail("Host binary:",std::string(g.host)+(fs::exists(root/g.host,ec)?" (found)":" (missing)"),
                   fs::exists(root/g.host)?col_ok:col_err);
            detail("Disc image:",fs::exists(root/"disc/1ST_READ.BIN",ec)?"disc/1ST_READ.BIN (found)":"disc/1ST_READ.BIN (missing)",
                   fs::exists(root/"disc/1ST_READ.BIN")?col_ok:col_warn);
            detail("Renderer:",s.renderer?"SDL3 / Vulkan":"raylib / OpenGL",col_muted);
            detail("Config:",(cfg/(std::string(g.id)+".conf")).string(),col_muted);
            const std::string log=latest_log(data,g.id);
            detail("Latest log:",log.empty()?"none yet":log,col_muted);

            ui_text(status,margin,h-72,18,status_err?col_err:col_ok);
            ui_text("Left/Right change   A select   B back",margin,h-28,15,col_hint);
        }

        // Clear-data confirmation, over everything.
        if(confirming_clear){
            const Game &cg=games[clear_game];
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
