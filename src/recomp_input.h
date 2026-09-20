/* Banjo rayplay/rayview control conventions adapted for native Dreamcast.
 * Source: /home/jon/rexglue-vmx/tools/rayview/main.cpp (input gathering).
 * Direct Maple delivery replaces Linux uinput; no elevated device access.
 */
static int input_mode; /* 0 external + keyboard, 1 virtual, 2 keyboard */
static int show_fps, overlay_visible=1;
static const unsigned pad_bits[]={4,2,0x400,0x200,8,0x10,0x20,0x40,0x80};
static const int pad_buttons[]={GAMEPAD_BUTTON_RIGHT_FACE_DOWN,GAMEPAD_BUTTON_RIGHT_FACE_RIGHT,GAMEPAD_BUTTON_RIGHT_FACE_LEFT,GAMEPAD_BUTTON_RIGHT_FACE_UP,GAMEPAD_BUTTON_MIDDLE_RIGHT,GAMEPAD_BUTTON_LEFT_FACE_UP,GAMEPAD_BUTTON_LEFT_FACE_DOWN,GAMEPAD_BUTTON_LEFT_FACE_LEFT,GAMEPAD_BUTTON_LEFT_FACE_RIGHT};
static Rectangle control_rect(int i) {
    float w=GetScreenWidth(),h=GetScreenHeight();
    const Vector2 xy[]={{w-110,h-78},{w-56,h-132},{w-164,h-132},{w-110,h-186},
        {w/2-48,h-70},{82,h-186},{82,h-78},{28,h-132},{136,h-132},
        {28,h-245},{w-120,h-245}};
    return (Rectangle){xy[i].x,xy[i].y,i==4?96:48,48};
}
static int control_down(int i) {
    Rectangle r=control_rect(i);
    if(IsMouseButtonDown(MOUSE_BUTTON_LEFT)&&CheckCollisionPointRec(GetMousePosition(),r))return 1;
    for(int j=0;j<GetTouchPointCount();j++)if(CheckCollisionPointRec(GetTouchPosition(j),r))return 1;
    return 0;
}
static void controls_init(void) {
    const char *mode=getenv("RECOMP_PAD");
    input_mode=mode&&!strcmp(mode,"virtual")?1:mode&&!strcmp(mode,"keyboard")?2:0;
    show_fps=getenv("RECOMP_SHOW_FPS")?atoi(getenv("RECOMP_SHOW_FPS")):1;
}
static void controls_draw(void) {
    static double start;static unsigned count;static float fps;
    double now=GetTime();count++;
    if(now-start>=1){fps=count/(now-start);count=0;start=now;}
    if(show_fps){
        DrawRectangle(10,10,230,30,(Color){10,18,28,210});
        DrawText(TextFormat("%.1f FPS | cap %s",fps,getenv("MSR_FPS")?getenv("MSR_FPS"):"off"),20,16,18,RAYWHITE);
    }
    if(input_mode!=1||!overlay_visible)return;
    const char *labels[]={"A","B","X","Y","START","UP","DN","LT","RT","L","R"};
    for(int i=0;i<11;i++){
        Rectangle r=control_rect(i);Color c=control_down(i)?(Color){30,200,160,200}:(Color){20,30,48,155};
        DrawRectangleRounded(r,0.4f,8,c);
        DrawText(labels[i],r.x+(r.width-MeasureText(labels[i],16))/2,r.y+16,16,WHITE);
    }
    DrawText("Virtual pad  |  F2 hide  |  Enter Start / Space A / Shift B / Q,R triggers",20,GetScreenHeight()-22,14,RAYWHITE);
}
static int selected_pad(void) {
    const char *name=getenv("RECOMP_GAMEPAD");
    for(int i=0;i<16;i++)if(IsGamepadAvailable(i)){
        if(!name||!*name||!strcmp(GetGamepadName(i),name))return i;
    }
    return -1;
}
