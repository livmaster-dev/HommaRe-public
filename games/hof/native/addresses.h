/* games/hof/native/addresses.h - Hammers of Fate's addresses for the shared
 * native code (native/touch.c, save.cpp, camera.c, horplus.c, ui_bounds.c), which the
 * build passes it to as RECOMP_ADDRESS_HEADER (game.toml [translate] native
 * addresses); the translated chunks never include it, so changing one only
 * rebuilds those sources. Found with games/map_addresses.py from the base
 * game's (native/touch.c): functions and data through byte-identical
 * functions, vtables through RTTI. */
#pragma once

#define HOMM5_CURSOR_OBJECT 0x0114bbe0 /* NUI::CCursor */
#define HOMM5_CURSOR_VTABLE 0x00fa89e4
#define HOMM5_CURSOR_X 0x0114bbd4
#define HOMM5_CURSOR_Y 0x0114bbd8
#define HOMM5_CURSOR_ID 0x0114bc28
/* CCursor::Draw and its hardware-cursor flag: the getter is vtable slot 10
 * here (00727c80), one later than the base game's. */
#define HOMM5_FN_CURSOR_DRAW fn_00727740
#define HOMM5_CURSOR_HW 0x0114bbc0
#define HOMM5_HOVER_TIMEOUT 0x010cff5c /* Options.Tooltips.HoverTimeout, 1.0 */
#define HOMM5_SCREEN_STACK 0x0114ab38
/* The IScreen subobjects sit earlier in the 2.1 screens' layouts. */
#define HOMM5_VT_ADVENTURE 0x00fb0c94
#define HOMM5_VT_COMBAT 0x00fad674
#define HOMM5_SUB_ADVENTURE 0x778
#define HOMM5_SUB_COMBAT 0x53c
#define HOMM5_SCREEN_WINDOWS 0x98 /* checked: the adventure init reads it where the base reads 0xa0 */
#define HOMM5_WINDOWS_MODAL 0x1f4
#define HOMM5_SAVE_SETTINGS 0x006f27b0 /* the base game's 006bfe80 */

/* The adventure map's camera (native/camera.c): what CCameraOperator::Update
 * (007df8b0, the base game's 007f26b0) reaches, mapped as above; the screen
 * fields from the adventure screen's init. */
#define HOMM5_CAM_VTABLE 0x00fb4a50       /* CCamera */
#define HOMM5_CAM_OPERATOR_VTABLE 0x00fb4e88 /* CCameraOperator */
#define HOMM5_SCREEN_CAMERA 0x9c
#define HOMM5_SCREEN_OPERATOR 0x568
#define HOMM5_VIRTUAL_SCREEN 0x010d4e24 /* the UI's virtual screen size, floats: read by GetVirtualSize 00e437a0 */
#define HOMM5_CAMERA_UPDATE_ORIG fn_007df8b0

/* The battle camera's start (native/camera.c): CArenaCameraConfigurator
 * (vtable 0xfb48e4) slot 8, found through its RTTI. */
#define HOMM5_BATTLE_DEFAULTS_ORIG fn_007d5ee0
/* The battle screen's camera operator (native/camera.c zoom_view_camera): the
 * battle's camera setup 0074c2b0 repeats the base game's 0074bcd0 call for
 * call and keeps the operator, made by CCameraOperator's constructor 007dde30
 * (found by its vtable 0xfb4e88), here; the base game's +0x4e4. */
#define HOMM5_COMBAT_OPERATOR 0x3a4

/* Hor+ (native/horplus.c): the projection 009ffda0 has the base game's
 * 00a1a1e0 arguments (h/w, fov, znear, zfar, offset) but always keeps the
 * horizontal angle; there is no wide-screen angle function to pass through. */
#define HOMM5_FN_PROJECTION fn_009ffda0

/* Interface bounds and stretched roots (native/ui_bounds.c): the base game's
 * axis 00e43650 is 00e4eba0 here, asked for x from SetParentRect 00e476c0 at
 * 00e477ce; the screen spans hof_screen_span (native/hof_screen.c). */
#define HOMM5_FN_ANCHOR_AXIS fn_00e4eba0
#define HOMM5_RET_ANCHOR_X 0x00e477d3
#define HOMM5_RET_ANCHOR_Y 0x00e4781d /* and for y (the dragged icon, native/ui_bounds.c; the phone rows, native/phone_layout.c) */
/* native/phone_layout_table.h: no wait button in a battle. */
#define HOMM5_PHONE_LAYOUT_HOF 1
#define HOMM5_SCREEN_SPAN hof_screen_span
#define HOMM5_STRETCH_ROOTS 1
/* A battle's stack counter (native/hof_screen.c hof_counter_window). */
#define HOMM5_COUNTER_WINDOW hof_counter_window
/* A window's SetPlacement (the base game's 00e3b9f0). */
#define HOMM5_FN_SET_PLACEMENT fn_00e479c0
/* Screen mode changes (native/hof_screen.c): the roots are laid out again
 * after one (native/phone_hud.c). */
#define HOMM5_SCREEN_MODE_CHANGES hof_screen_mode_changes

/* The phone's interface (native/phone_hud.c), from the base game's through
 * byte-identical functions, RTTI and the classes' sub-object offsets
 * (research notes phone-hud-expansions-addresses.md, 2026-10-19). "probable": the
 * town's, hire, build, hero and loading screens' IScreen vtables and offsets
 * (by structure), the clipped draw 00e51010 and the loading screen's
 * constructor 0082ef90; the rest hash-identical. This game has no root
 * placement (the base game's 00e74f30): a root takes its 1024x768 rect as it
 * is described (00e7f630), the safe area goes into its edge windows' anchors
 * (native/ui_bounds.c), and its conversions have no virtual size in memory. */
#define HOMM5_VT_TOWN 0x00fc407c /* NUI::CTownScreen's IScreen, at +0x484 */
#define HOMM5_SUB_TOWN 0x484
#define HOMM5_VT_LOADING 0x00fb87bc /* NUI::CPWLScreenInterface's, at +0x1c0 */
#define HOMM5_SUB_LOADING 0x1c0
#define HOMM5_VT_HIRE 0x00fc37ac /* NUI::CHireScreen's, at +0x27c */
#define HOMM5_SUB_HIRE 0x27c
#define HOMM5_VT_BUILD 0x00fc96f8 /* NUI::CTownBuildScreen's, at +0x258 */
#define HOMM5_SUB_BUILD 0x258
#define HOMM5_VT_HERO 0x00fb926c /* NUI::CHeroScreen2's, at +0x794 */
#define HOMM5_SUB_HERO 0x794
#define HOMM5_VT_LOADING_MAIN 0x00fb8758 /* NUI::CPWLScreenInterface */
#define HOMM5_VT_MS_BUTTON 0x01032cf8
/* The drop-down list of a combo box (the options' screen resolution: it is
 * placed under its field from the interface's left edge, native/ui_bounds.c). */
#define HOMM5_VT_COMBO_LIST 0x01033278
#define HOMM5_VT_EDIT_LINE 0x01036dec
#define HOMM5_VT_EDIT_BOX 0x0103c300
#define HOMM5_VT_LIST 0x01033278
#define HOMM5_VT_SCROLLER 0x01037c40
#define HOMM5_VT_TEXT_VIEW 0x01038670
#define HOMM5_HUD_ROOT_RECT 1
#define HOMM5_UI_REAL_W 0x010d4e2c /* the effective width native/hof_screen.c keeps there */
#define HOMM5_UI_REAL_H 0x010d4e30
#define HOMM5_UI_ORIGIN_X 0x012875bc
#define HOMM5_UI_ORIGIN_Y 0x012875c0
#define HOMM5_FN_WINDOW_DRAW fn_00e49cb0
#define HOMM5_FN_WINDOW_CONTAINS fn_00e47cd0
#define HOMM5_FN_SIMPLE_DRAW fn_00e51010
#define HOMM5_FN_TEXT_LAYOUT fn_00e4f990
#define HOMM5_ADDR_TEXT_LAYOUT 0x00e4f990u
#define HOMM5_FN_LOADING_SCREEN fn_0082ef90
#define HOMM5_FN_ROOT_DESCRIBE fn_00e7f630
#define HOMM5_SCREEN_DESC 0xc4 /* where 00e7f630 keeps the description (the base game's +0xc8) */
/* SetParentRect (thiscall: ECX the window, the parent's rect on the stack):
 * a screen laid out again when the camera housing changes sides. */
#define HOMM5_ADDR_SET_PARENT_RECT 0x00e476c0u

/* The touch camera's ground pick (native/camera.c): the 3D picture has the
 * screen's own shape, whatever the virtual size says. */
#define HOMM5_CAMERA_PICTURE_SHAPE 1
/* CSkeletonAnimator's NeedUpdate, the translated original (base 0067c060). */
#define HOMM5_ANIM_NEED_UPDATE_ORIG fn_006a5a70

/* native/overrides.c: the x87 float max/min helpers (the base game's
 * 00614540 / 00614570); no counterpart of the box grow 0066e070. */
#define HOMM5_FLOAT_MAX_ORIG fn_0063d930
#define HOMM5_FLOAT_MIN_ORIG fn_0063d960

/* native/scene.c: the index buffer's fill and its "no base" flag, a vertex
 * pool's range, the cull tests and their sign mask (the base game's
 * 0060d260 / 0x011375e8, 0060d810, 00a1a680 / 00a1a9f0 / 00a1ab50,
 * 0x00f94980). */
#define HOMM5_IB_FILL_ORIG fn_00636690
#define HOMM5_IB_NOBASE 0x01136ce0u
#define HOMM5_POOL_ALLOC_ORIG fn_00636c40
#define HOMM5_CULL_TEST_ORIG fn_00a00280
#define HOMM5_CULL_PUSH_ORIG fn_00a005f0
#define HOMM5_CULL_OBJECT_ORIG fn_00a00750
#define HOMM5_CULL_SIGNMASK 0x00f9d6c0u

/* native/granny.c: granny2.dll is the base game's, the defaults there. */
/* native/lighting.c: 00698d00 (the base game's 0066f2e0) and its reciprocal
 * square root table. */
#define HOMM5_LIGHT_VERTEX_ORIG fn_00698d00
#define HOMM5_LIGHT_RSQRT 0x01138900u
/* native/gpu_skin.cpp (the base game's in brackets): the part fill
 * (0066e4d0), the staged copy (0066d950), the format 5 allocation
 * (00606af0), the alpha 006b9670 (0068fc20) and its vectors (01138ee8 /
 * 01138ef4), the tables (01139088 / 01142088), gfx_low_ram (010cd1f9) and
 * the bounds' positions (0066f600). */
#define H5SKIN_FILL_ORIG fn_00697ef0
#define H5SKIN_COPY_ORIG fn_00697370
#define H5SKIN_ALLOC_ORIG fn_0062ffa0
#define H5SKIN_ALPHA_FN 0x006b9670u
#define H5SKIN_ALPHA_A 0x01138760u
#define H5SKIN_ALPHA_B 0x0113876cu
#define H5SKIN_RSQRT 0x01138900u
#define H5SKIN_CBRT 0x01141900u
#define H5SKIN_LOW_RAM 0x010cf1e1u
#define H5SKIN_POSITIONS_ORIG fn_00699020

/* The end of a frame's 3D world (native/render_split.c): CGameView::Draw
 * (the NGScene::CGameView vtable's slot 0x50) and where the world pass gets
 * back from it - its main, second and alternate view. */
#define HOMM5_FN_GAME_VIEW_DRAW fn_006565e0
#define HOMM5_RET_VIEW_MAIN 0x007bcbe1u
#define HOMM5_RET_VIEW_SECOND 0x007bcc0du
#define HOMM5_RET_VIEW_ALT 0x007bc912u

/* Native screens (native/native_screens.c, screens_*.c): research notes phone-menus/native/
 * expansion-hof-addresses.md. */
/* ---- native screens (native/native_screens.c, screens_*.c): Hammers of Fate 2.1 ----
 * research notes phone-menus/native/expansion-hof-addresses.md. V = seen in HoF code,
 * I = inferred. */
/* native_screens.c */
#define NS_SCREEN_STACK 0x0114ab38      /* V (44 votes; = HOMM5_SCREEN_STACK) */
#define NS_DIFFICULTY 0x00b09fd0        /* V hash (base 00b97bd0) */
#define NS_TEXT 0x009f0330              /* V hash (base 00a09de0) */
#define NS_FREE 0x0060de20              /* V call pairs (base 005e40c0) */
/* also needed, now constants in the shared code (§2):
 * NS_SCREEN_ROOT 0x98, NS_ROOT_DRAW 0x14, IWindow FindChild 0x94, caption 0xbc */

/* screens_custom.c */
#define NS_VT_CREATE 0x00fcca8c         /* V RTTI CCustomGameCreateScreen IScreen */
#define NS_SUB_CREATE 0x1c0             /* V COL offset (base 0x1c4) */
#define NS_VT_WAIT 0x00fcee3c           /* V RTTI CCustomGameWaitScreen IScreen */
#define NS_SUB_WAIT 0x2b0               /* V (base 0x2c4) */
#define NS_NEW 0x0060de10               /* V call pairs (base 005e40b0) */
#define NS_CREATE_REACTION 0x0099f8f0   /* V reactions vt 00fcca64 slot 0; RET 0xc; ECX = screen+0x168 */
#define NS_CREATE_REFRESH 0x009a04a0    /* V called by the reaction for time_limit/difficulty */
#define NS_MAPS_SORT 0x009b9160         /* V same 570 insns (only IWindow slot +8), RET 4 */
#define NS_MAPS_SELECTED 0x009baf10     /* V hash */
#define NS_MAP_NAME 0x009b8550          /* V reads desc+0x4c (base +0x54) */
#define NS_MAP_DESC 0x009b83b0          /* V reads desc+0x58 (base +0x60) */
#define NS_MAP_GOAL 0x009b8460          /* V hash */
#define NS_MAP_PLAYERS 0x009bb2d0       /* V hash */
#define NS_MAP_SIZE 0x00734ea0          /* V hash */
#define NS_MP_MAPS 0x0115dc14           /* V the reaction toggles it on "mp_filter_changed" */
#define NS_SLIDER_VT 0x01037e8c         /* V RTTI CWindowSlider +0xf8 */
#define NS_WAIT_REACTION 0x009ce8f0     /* V reactions vt 00fcee0c slot 0; RET 0xc */
#define NS_VT_HS_CREATE 0x00fccc24      /* V RTTI CHSCreate IScreen */
#define NS_SUB_HS_CREATE 0x18c          /* V (base 0x194) */
#define NS_VT_HS_WAIT 0x00fcef98        /* V RTTI CHSWaitScreen IScreen */
#define NS_SUB_HS_WAIT 0x1b0            /* V (base 0x1bc) */
#define NS_HS_CREATE 0x009a17b0         /* V registered for "create" by 009a14c0; RET 4 */
#define NS_HS_LOAD 0x009a1b10           /* V "load"; RET 4 */
#define NS_HS_BACK 0x009a1b40           /* V "back"; RET 4 (also marks the screen below, [x+0xbc]=1) */
#define NS_HS_MAPSELECTED 0x009a1ba0    /* V "mapselected"; RET 4. (HoF adds "rmg_screen" -> 009a1bd0) */
#define NS_HS_START 0x009d1670          /* V registered by 009d1230 (hash = base 009f18f0) */
#define NS_HS_WAIT_BACK 0x009d1650      /* V */
#define NS_HS_NAME_ACCEPT 0x009d16f0    /* V "edit_accept" */
#define NS_RESOURCE_OF 0x00a1d5b0       /* V hash */
#define NS_DYNAMIC_CAST 0x009e5205      /* V (__RTDynamicCast, same call pattern) */
#define NS_RTTI_RESOURCE 0x010e9990     /* V TD ".?AVCResource@NDb@@" */
#define NS_RTTI_HERO_SHARED 0x010eb674  /* V TD ".?AUSAdvMapHeroShared@NDb@@" */
#define NS_TEXT_REF 0x009f0330          /* V */

/* screens_menu.c */
#define NS_VT_MENU 0x00fbf508           /* V RTTI CMainMenuScreen IScreen */
#define NS_SUB_MENU 0x1f8               /* V (base 0x214) */
#define NS_MENU_REACTION 0x008b0a30     /* V hash; ECX = screen+0x168 */
#define NS_MENU_SWITCH 0x008b0df0       /* V vt 00fbf4c0 slot 0; ECX = screen+0x170; RET 4; index 0..9 only */
#define NS_MENU_CREDITS 0x00947150      /* V queues CCreateCreditsScreenCmd (vt 00fc8340) */
/* V the main menu's "options" handler (registered by name in 008b1b90); ECX =
 * screen, RET 4 (the argument unused): switch(3), which in HoF only hides the
 * panel (no case 3, base 008ad570 opens Options there), then the options'
 * setup and CCreateGameOptionsScreen queued (00878380). */
#define NS_MENU_OPTIONS 0x008b1210
#define NS_MENU_EXIT 0x00820030         /* V hash */
#define NS_PROFILE_VALID 0x0093f940     /* V hash */
#define NS_PROFILE_NAME 0x0093f990      /* V hash */
#define NS_PROFILE_SHOWN 0x0093f920     /* V hash */
#define NS_PROFILE_SHOW 0x0093f860      /* V hash */
#define NS_PROFILE_DELETE 0x0093ea30    /* V hash */
#define NS_SETUP_REACTION 0x0094a230    /* V hash */
#define NS_DIFFICULTY_NOW 0x0115bcf8    /* V same operand in 2 hash-identical fns (004c6d20, 009613b0) */
#define NS_CAMPAIGN_OF 0x00943c20       /* V same code; calls set vt+8 (base +0xc) */
#define NS_VT_EDIT_BOX 0x00fcb610       /* V RTTI CSimpleEditBox IScreen */
#define NS_SUB_EDIT_BOX 0x1ac           /* V (base 0x1b8) */
#define NS_VT_FIRST_PROFILE 0x00fc7bdc  /* V RTTI CSelectFirstProfileNameScreen sub at +0x168 (base +0x174) */
#define NS_EDIT_BOX_OK 0x009801d0       /* V 84/85 insns, box+0x170/0x174 */
#define NS_TEXT_OF_REF 0x009f0330       /* V */

/* screens_load.c */
#define NS_VT_LOAD 0x00fa97fc           /* V RTTI CLoadScreen<SHeaderInfo> IScreen */
#define NS_SUB_LOAD 0x1d4               /* V (base 0x2b4: the class is 0xe0 smaller) */
#define NS_LOAD_CLICK 0x00749000        /* V the twin called by primary vt slot 0x48 00737390 ("click") */
#define NS_LOAD_LOAD 0x00738b50         /* V primary vt slot 0x3c (base slot 0x40) */
#define NS_LOAD_CONFIRMED 0x00738ce0    /* V IMsgBoxNotify vt 00fa97cc slot 0; ECX = screen+0x168 */
#define NS_LOAD_CLOSE 0x00746780        /* V same body (close-top + defer, AL=1, RET 4), registered at the same spots */
#define NS_LOAD_TYPE 0x00734b50         /* V hash */
#define NS_LOAD_SIZE 0x00734ea0         /* V hash */
#define NS_DIFF_OF_SAVE 0x00b0a010      /* V hash */
#define NS_DIFF_ENTRY 0x00b09fd0        /* V hash */

/* screens_hof.c (Hall of Fame) */
#define NS_VT_HOF 0x00fc7920            /* V RTTI CHallOfFameScreen IScreen */
#define NS_SUB_HOF 0x1c0                /* V (base 0x1cc) */
#define NS_HOF_REACTION 0x0093bc20      /* V same 197 insns; RET 0xc */
#define NS_HOF_PICK 0x0093c330          /* V same 684 insns; RET 8 */
#define NS_HOF_CLEAR 0x0093cd40         /* V vt 00fc78f0 slot 0; ECX = screen+0x170 */
#define NS_HOF_CLEAR_OK 0x17363b00      /* V same constant compared */

/* screens_options.c */
#define NS_VT_OPTIONS 0x00fbb848        /* V RTTI CGameOptionsScreen IScreen */
#define NS_SUB_OPTIONS 0x1e0            /* V (base 0x1f8) */
#define NS_OPT_SHOW 0x0087a8b0          /* V */
#define NS_OPT_CHANGED 0x00879b40       /* V */
#define NS_OPT_ACCEPT 0x0087a530        /* V */
#define NS_OPT_PAGE_BACK 0x0087a800     /* V */
#define NS_OPT_CLOSE 0x0087ab20         /* V */
#define NS_OPT_DEFAULT 0x0087ac20       /* V vt 00fbb818 slot 0; ECX = screen+0x170 */
#define NS_OPT_DEFAULT_OK 0x172a6c00    /* V same constant */
#define NS_OPT_REVERT 0x008797b0        /* V hash */
#define NS_VT_OPT_HEADER 0x00fbb8bc     /* V RTTI SHeaderOption */
#define NS_VT_OPT_CHECK 0x00fbb8ec      /* V RTTI SCheckBoxOption */
#define NS_VT_OPT_COMBO 0x00fbb91c      /* V RTTI SComboBoxOption */
#define NS_VT_OPT_SLIDER 0x00fbb94c     /* V RTTI SSliderOption */

/* screens_duel.c: not enabled; the HoF duel and 3x3 hero views differ (report §3, §5). */

/* screens_question.c (+ overrides.h) */
#define NS_VT_MSGBOX 0x00fb38a4         /* V RTTI CCommonMsgBox, 13 slots agree */
#define NS_VT_MSGBOX_TEXT 0x00fbbbcc    /* V RTTI CCommonMsgBoxWithUpdatableText */
/* games/hof/native/overrides.h: void homm5_msgbox_setup(X86 *c); #define FN_007c3910 homm5_msgbox_setup
 * (base 007c9d90; same 44 insns, RET 0x18). screens_question.c calls fn_007c9d90 by name, so it needs a macro
 * (e.g. NS_MSGBOX_SETUP_ORIG fn_007c3910). V */

/* screens_escmenu.c */
#define NS_VT_ESC_MENU 0x00fb7a2c       /* V RTTI CEscMenuScreen IScreen */
#define NS_SUB_ESC_MENU 0x1d0           /* V (base 0x1dc) */
#define NS_ESC_REACTION 0x0081e680      /* V reactions vt 00fb79f4 slot 0; RET 0xc */
#define NS_MSGBOX_SETUP_ORIG fn_007c3910 /* the translated box setup (overrides.h) */

/* ---- fields and slots that differ from the base game (report §2, §7) ---- */
#define NS_SCREEN_ROOT 0x98
#define NS_IW_FIND 0x94
#define NS_IW_CAPTION 0xbc
#define NS_SLIDER_SET_SEGMENT 0x28
#define NS_SLIDER_GET_SEGMENT 0x2c
/* screens_custom.c */
#define NS_SCREEN_REACTIONS 0x168
#define NS_C_MAPS 0x174
#define NS_C_TIME 0x188
#define NS_C_DIFFICULTY 0x190
#define NS_HC_MAPS 0x168
#define NS_HC_OPTIONS 0x170
#define NS_W_INFO 0x184
#define NS_W_TIME 0x1fc
#define NS_W_ROWS 0x20c
#define NS_W_CONTROLLER 0x218
#define NS_W_MODEL 0x21c
#define NS_W_DIFFICULTY 0x294
#define NS_HW_LOGIC 0x18c
#define NS_HL_MODEL 0x100
#define NS_ROW_NAME_EDIT 0x134
#define NS_M_RECORD_SIZE 0x60
#define NS_M_CAN_TYPE 0x68
#define NS_M_CAN_BONUS 0           /* no such slot */
#define NS_R_ARTIFACT_OK 0         /* no such rule */
#define NS_HS_SPEC 0x10c
#define NS_HS_NAME 0x16c
#define NS_HS_FACE_SMALL 0x134
#define NS_HS_FACE_BIG 0x12c
#define NS_RACE_FIRST 3            /* Haven 3 .. Inferno 8, Fortress 9 */
#define NS_RACE_RANDOM 1
#define NS_NO_COMPUTER_RULE 1
/* screens_menu.c */
#define NS_M_REACTIONS 0x168
#define NS_M_BASE 0x170
#define NS_M_MAIN 0x1b0
#define NS_M_SINGLE 0x1b4
#define NS_M_MULTI 0x1b8
#define NS_M_INDEX 0x1bc
#define NS_M_MISSIONS 0x1c4
#define NS_M_HOT_SEAT 0x1d4
#define NS_M_USER_CAMPAIGNS 0      /* none in HoF */
#define NS_M_CAMPAIGNS 0x1c0
#define NS_BOX_CALLBACK 0x170
#define NS_BOX_EDIT 0x174
#define NS_SET_GET 0x08
#define NS_SET_UNLOCKED 0x0c
#define NS_CAMPAIGN_COUNT 0x0c
#define NS_CAMPAIGN_MISSION 0x10
#define NS_CAMPAIGN_DONE 0x14
#define NS_CAMPAIGN_AVAILABLE 0x18
/* Campaigns A1C1..A1C3: a mission's loading picture is PWLPic (10 of them). */
#define NS_MISSION_PICTURE(out, cap, key, i) snprintf(out, cap, "/Maps/Scenario/%sM%d/PWLPic.dds", key, (i) + 1)
#define NS_GAME_VERSION "2.1"
/* screens_load.c */
#define NS_L_REACTIONS 0x168
#define NS_L_CONTAINER 0x170
#define NS_L_SELECTED 0x17c
#define NS_L_HEADERS 0x180
#define NS_L_READING 0x1b0
#define NS_L_CAN_DELETE 0          /* no such flag */
/* screens_hof.c */
#define NS_HF_REACTIONS 0x168
#define NS_HF_BOXES 0x170
#define NS_HF_DATA 0x18c
#define NS_HF_BOOK 0x190
#define NS_HF_ENTRY 0x194
/* screens_options.c */
#define NS_O_BOXES 0x170
#define NS_O_SHOWN 0x194
#define NS_O_CATEGORIES 0x198
#define NS_O_MODE 0x1b0
/* screens_question.c */
#define NS_B_YESNO 0x138
#define NS_S_BOXES 0x144
/* screens_escmenu.c */
#define NS_E_REACTIONS 0x168
#define NS_E_MAIN 0x178
#define NS_E_MODE 0x1b4
#define NS_PAK_DIRS "data,dataa1" /* the paks it mounts, for the native views' pictures */

/* GameSpy clDataList string readers and the fixed buffers of all 52 calls of
 * the unbounded one (native/net_guard.c). The unbounded reader is the base
 * game's 004432d0, the bounded one 00443390; PLAYERINFO's seven fields
 * (0042a520, the base game's 0042a5e0) go into 12/16/36/36/68/132/132. The
 * sites are in the base game's order (net_guard.c names each), the sizes the
 * same. */
#define HOMM5_GS_STR_ORIG fn_00443240
#define HOMM5_GS_STR_BOUNDED fn_00443300
#define HOMM5_GS_STR_SITES \
    {0x0042a5e8u, 12}, {0x0042a5fcu, 16}, {0x0042a610u, 36}, {0x0042a624u, 36}, {0x0042a638u, 68}, \
        {0x0042a64cu, 132}, {0x0042a660u, 132}, {0x0042497cu, 36}, {0x004249bfu, 132}, \
        {0x004249dau, 132}, {0x00424f60u, 36}, {0x00424f9fu, 132}, {0x00424fb6u, 132}, \
        {0x0043f498u, 16}, {0x0043f4aeu, 132}, \
        {0x0042723au, 36}, {0x004272b4u, 36}, {0x0042733fu, 132}, {0x0042383eu, 132}, \
        {0x00423876u, 132}, {0x004222bau, 132}, {0x00428cafu, 132}, {0x00430434u, 36}, \
        {0x00425f0au, 16}, {0x004233e6u, 16}, {0x004259b0u, 36}, {0x004260c7u, 16}, \
        {0x0041fc01u, 36}, {0x0041fc17u, 36}, {0x0041fc2du, 128}, {0x0041fc43u, 128}, \
        {0x00421ebdu, 129}, {0x0042201du, 129}, {0x0042253cu, 36}, {0x0042257du, 36}, \
        {0x004227d9u, 16}, {0x004227edu, 129}, {0x004227fdu, 129}, {0x0042291fu, 16}, \
        {0x00422aafu, 16}, {0x00423b2fu, 16}, {0x00423c8fu, 16}, {0x00425cc8u, 16}, \
        {0x00425cf8u, 16}, {0x004276cau, 36}, {0x00427d87u, 33}, {0x00427f60u, 36}, \
        {0x00429649u, 16}, {0x00429682u, 132}, {0x0042975fu, 16}, {0x00429773u, 36}, \
        {0x0042979au, 1024},

/* The GS login (native/gs_login.c): the SDK's login entry, reached from
 * ProcessLogin (00e06a10) through the thunk 00419340, and
 * ProcessLoginRouterResult. */
#define HOMM5_FN_GS_LOGIN fn_00419a30
#define HOMM5_FN_GS_LOGIN_RESULT fn_00e06e30

/* ---- ubi.com screens (native/screens_ubi.c): Hammers of Fate 2.1 ----
 * research notes lan/ubi-screens-expansions.md. Base 1.60 value in the comment. */
#define NS_UBI_VT_LOGIN         0x00fc6150  /* base 00fc2cb0; V RTTI CMPRegisterScreen IScreen */
#define NS_UBI_SUB_LOGIN        0x1d0       /* base 0x1e0; V (alloc 0x1d8 in 0091f7d0) */
#define NS_UBI_LOGIN_OK         0x0091e250  /* base 00947010; V reg "ok" in ctor 0091d570, RET 4 */
#define NS_UBI_LOGIN_CANCEL     0x0091e620  /* base 009473e0; V reg "cancel"/"esc_pressed" → 0091d400 (logout + main menu) */
#define NS_UBI_VT_EDIT_BOX      0x00fcb610  /* base 00fc83e8; V (= NS_VT_EDIT_BOX; the .c falls back to it) */
#define NS_UBI_SUB_EDIT_BOX     0x1ac       /* base 0x1b8; V (= NS_SUB_EDIT_BOX) */
#define NS_UBI_BOX_CALLBACK     0x00fc60e8  /* base 00fc2c48; V RTTI ISimpleEditBoxCallBack @0x170; CD key box built with ECX = screen+0x170 (0091e213) */
#define NS_UBI_BOX_OK           0x009801d0  /* base 009a7b80; V (= NS_EDIT_BOX_OK) */
#define NS_UBI_BOX_CANCEL       0x00980120  /* base 009a7ad0; V (lan-screens-hof.md) */
#define NS_UBI_VT_CHANNELS      0x00fbb398  /* base 00fb70f0; V RTTI CMPChangeChannelScreen IScreen */
#define NS_UBI_SUB_CHANNELS     0x1d4       /* base 0x1e0; V (alloc 0x1dc in 00875610) */
#define NS_UBI_CHANNELS_JOIN    0x00873de0  /* base 0088e7a0; V reg "join" in ctor 00873250, RET 4 */
#define NS_UBI_CHANNELS_CANCEL  0x00873d80  /* base 0088e740; V reg "cancel", RET 4 */
#define NS_UBI_VT_WAIT_BOX      0x00fb3294  /* base 00fadd04; V RTTI CWaitBox */
#define NS_UBI_RTTI_IWINDOW     0x010ebfb0  /* base 010e9ab0; V (pushed by IListControl vt+0x24 00e56d80) */
#define NS_UBI_RTTI_LIST_ITEM   0x010efb84  /* base 010ed854; V (same) */
#define NS_UBI_RTTI_OBJECT      0x010e90a8  /* base 010e67a0; V (join 00873ea8) */
#define NS_UBI_RTTI_CHANNEL     0x010efa88  /* base 010ed774; V TD CChannelData (join 00873ea3) */
#define NS_UBI_DYNAMIC_CAST     0x009e5205  /* base 009fe372; V */
/* inner offsets that differ (the .c's #ifndef defaults are base) */
#define NS_UBI_UL_LOGIN         0x19c       /* base 0x1a8; V init 0091da00 "login", OK 0091e269 */
#define NS_UBI_UL_PASSWORD      0x1a0       /* base 0x1ac; V init "password", OK 0091e423 */
#define NS_UBI_UL_LOGGING       0x1a4       /* base 0x1b0; V OK 0091e4ea (CUbiLoginListener) */
#define NS_UBI_UL_WAIT          0x1a8       /* base 0x1b4; V OK 0091e359, init-start 0091d399 */
#define NS_UBI_UL_INIT          0x1ac       /* base 0x1b8; V init-start 0091d2eb (CUbiInitListener) */
#define NS_UBI_UL_FORM          0x1b0       /* base 0x1bc; V init "mp_register", SetVisible(0) at 0091da00 */
#define NS_UBI_UB_CALLBACK      0x170       /* base 0x17c; V (CSimpleEditBox, lan-screens-hof.md §2.2) */
#define NS_UBI_UB_EDIT          0x174       /* base 0x180; V */
#define NS_UBI_UC_LIST          0x188       /* base 0x194; V init 00873550 "channels-table" */
#define NS_UBI_UC_JOIN          0x1b4       /* base 0x1c0; V init "ButtonJoin" */
#define NS_UBI_UC_WAIT          0x1b8       /* base 0x1c4; V join 00873fbc; join-ok reads [ecx(+0x170)+0x48] */
/* same as base (no define needed): CH_NAME 0x0c, CH_MODE 0x18, CH_PLAYERS 0x24 (V fill 008741b0);
 * WAIT_BOX_FROM 0xf8 (V 007c3793), WAIT_BOX_NOTIFY 0x108 (V cancel 007c5940); EDIT_TEXT 0x3c (V 006aab30);
 * LIST_ROWS 0x20 / LIST_SELECTED 0x24 / LIST_SELECT 0x28 (V 00e56d60/80/a0); ITEM_DATA 0x04 (V 00873eb5). */

/* ---- lobbies in LAN and ubi.com mode + name box (native/screens_lan.c) ---- */
#define NS_LAN_VT_LOBBY         0x00fc5b00  /* base 00fc2180; V RTTI CLobbyScreen */
#define NS_LAN_SUB_LOBBY        0x208       /* base 0x200; V */
#define NS_LAN_VT_DUEL_LOBBY    0x00fc5d08  /* base 00fc237c; V RTTI CMPDMLobbyScreen */
#define NS_LAN_SUB_DUEL_LOBBY   0x200       /* base 0x1f8; V */
#define NS_LAN_LOBBY_REACTION   0x00916f00  /* base 0093b320; V re-checked: ECX = screen+0x168, RET 0xc, falls through to 00974670 */
#define NS_LAN_DUEL_LOBBY_REACTION 0x009188e0 /* base 0093cda0; V re-checked (same) */
#define NS_LAN_SELECTED         0x009b6b50  /* base 0099a1a0; V hash, fastcall ECX = games view */
#define NS_LAN_RTTI_IWINDOW     0x010ebfb0  /* base 010e9ab0; V */
#define NS_LAN_RTTI_LIST_ITEM   0x010efb84  /* base 010ed854; V */
#define NS_LAN_RTTI_OBJECT      0x010e90a8  /* base 010e67a0; V */
#define NS_LAN_RTTI_GAME_DATA   0x010f3ce0  /* base 010f0b6c; V */
#define NS_LAN_RTTI_PLAYER_DATA 0x010f3b70  /* base 010f17d0; V */
#define NS_LAN_BOX_CALLBACK_STD 0x00fc5aa8  /* base 00fc2134; V */
#define NS_LAN_BOX_CALLBACK_DUEL 0x00fc5cb4 /* base 00fc2334; V */
#define NS_LAN_BOX_OK           0x009801d0  /* base 009a7b80; V */
#define NS_LAN_BOX_CANCEL       0x00980120  /* base 009a7ad0; V */
#define NS_LAN_DYNAMIC_CAST     0x009e5205  /* base 009fe372; V */
#define NS_LAN_TEXT             0x009f0330  /* base 00a09de0; V (= NS_TEXT) */
#define NS_LAN_MAP_NAME         0x009b8550  /* base 009d9400; V the formatter calls it with ECX = info+0xcc (009b6d98) */
#define NS_LAN_MAP_SIZE         0x00734ea0  /* base 00729b30; V (= NS_MAP_SIZE) */
/* NS_LAN_SPEED_NAME: none (no combat speed in HoF, §3.1). The .c needs a guard. V */
#define NS_LAN_SERVICES         0x00611ab0  /* base 005e8050; V re-checked (lobby init 0097577b, login cancel 0091d405) */
#define NS_LAN_MATCHMAKER_ID    0x8c348b00  /* same; V */
#define NS_LAN_CONTAINER_MODE   0x20        /* same; V re-checked (lobby init 00975784, entry-name 00975dd4) */
/* lobby fields (CLobbyScreenBase) */
#define NS_LAN_L_REACTIONS      0x168       /* base 0x174; V re-checked */
#define NS_LAN_L_CHAT           0x198       /* base 0x19c; V */
#define NS_LAN_L_PLAYERS        0x1a0       /* base 0x1a4; V re-checked (player_profile [ebx+0x38]) */
#define NS_LAN_L_PROFILE        0x1a4       /* base 0x1a8; V re-checked (init "MenuProfile" store 0097599d) */
#define NS_LAN_L_EDIT           0x1a8       /* base 0x1ac; V re-checked (chat [ebx+0x40]) */
#define NS_LAN_L_LEAVING        0x1ac       /* base 0x1b0; V re-checked (channel vt+0x48 00974ce0: CLobbyLeaveListener) */
#define NS_LAN_L_JOINING        0x1b0       /* base 0x1b4; V re-checked (join 00974a70) */
#define NS_LAN_L_JOIN           0x1b4       /* base 0x1b8; V re-checked */
#define NS_LAN_L_GAMES          0x1d0       /* base 0x1d4; V re-checked */
/* same as base: PLAYERS_LIST 0x38, GV_LIST 0x08, LIST_ROWS 0x20, LIST_SELECT 0x28, ITEM_DATA 0x04, GAME_INFO 0x0c;
 * GI_OPEN 0x18, GI_NAME 0x1c, GI_LOCKED 0x34 (now = password non-empty), GI_MAX 0x6c, GI_SIZE 0x70, GI_SAVED 0x90,
 * GI_MAP 0xcc, GI_ARENA 0xf8; CHAT_LINES 0x0c, CHAT_STRIDE 0x18, CHAT_TEXT 0x0c; PLAYER_NAME 0x0c; EDIT_TEXT 0x3c.
 * V (lan-screens-hof.md §2.4). */
/* GI_SPEED, GV_RUNNING, GV_OBSERVABLE: none (§3.1). */
/* the name / password box */
#define NS_LAN_BOX_CB           0x170       /* base 0x17c; V */
#define NS_LAN_BOX_EDIT         0x174       /* base 0x180; V */
#define NS_LAN_BOX_ID           0x17c       /* base 0x188; V */
/* HoF lacks (research notes lan/ubi-screens-expansions.md §3.1): */
#define NS_LAN_HAS_FILTERS      0           /* no show/hide_running, show/hide_observable, no CGamesView2 bytes; V */
#define NS_LAN_HAS_SPEED        0           /* no SGameInfo combat speed, no speed names; V */
#define NS_LAN_GI_PLAYERS       0x84        /* players in = [info+0x84], as HoF's own list reads it (009b6cf1); V */
#define NS_LAN_UBI_ONLY         1           /* the lobbies natively in ubi.com mode only */

/* ---- network game: create (ns_mp_create) and players (ns_mp_wait), Hammers of Fate 2.1 ----
 * research notes lan/mp-create-wait-expansions.md. Base 1.60 value in the comment. */
#define NS_VT_MPC              0x00fcafa0  /* base 00fc797c; V RTTI CMPCreateScreen IScreen */
#define NS_SUB_MPC             0x198       /* base 0x1a0; V (alloc 0x1a0) */
#define NS_MPC_MAPSELECTED     0x00977de0  /* base 00997fe0; V re-checked (reg in 00977eb0, RET 4) */
#define NS_MPC_CREATE          0x00977930  /* base 00997b30; V re-checked */
#define NS_MPC_LOAD            0x00977d70  /* base 00997f50; V re-checked (LAN load / ubi.com load by mode) */
#define NS_MPC_BACK            0x00977900  /* base 00997b00; V re-checked */
#define NS_MPC_RMG             0x00977e10  /* new; V `rmg_screen`: [screen+0x180] = 1, push CCreateRMGScreen (009c2250), RET 4 */
#define NS_MPC_MAPS            0x170       /* base 0x17c; V re-checked (mapselected, create) */
#define NS_MPC_OPTIONS         0x178       /* base 0x184; V re-checked */
#define NS_MPC_CREATING        0x17c       /* base 0x188; V re-checked (create listener) */
/* CMPCreateOptions (init 009bc1a0, re-checked) */
#define NS_MPO_NAME            0x10        /* same; V */
#define NS_MPO_PASSWORD        0x14        /* same; V (always enabled; non-empty = private game) */
#define NS_MPO_LOCKED          0           /* base 0x18 `lockedgame`: NONE (§3) */
#define NS_MPO_GHOST           0x18        /* base 0x1c; V `ghostmode` → info+0x35 */
#define NS_MPO_QUICK           0x1c        /* base 0x20 (`simultaneus` «Быстрый бой» → +0x36); V `quick_combat` «Быстрая схватка» → info+0x36 */
#define NS_MPO_AUTOSAVE        0x20        /* base 0x24; V `realtime`, HoF text «Динамичные бои» → info+0x37 (§3) */
#define NS_MPO_SIMULTANEOUS    0x3c        /* new; V `simultaneus` «Одновременные ходы» → info+0x154 */
#define NS_MPO_TIME            0x24        /* base 0x28; V `timelimitscroll` */
#define NS_MPO_DIFFICULTY      0x28        /* base 0x2c; V `difficultyscroll` */
#define NS_MPO_ANIMATION       0x34        /* base 0x38; V `animationspeedscroll` */
#define NS_MPO_COMBAT          0           /* base 0x40: NONE (no combat-turn-speed slider) */
/* NS_MPC_SPEED_NAME: none (no combat speed in HoF). */

#define NS_VT_MPW              0x00fbd9f4  /* base 00fbaae0; V RTTI CMPWaitScreen IScreen */
#define NS_SUB_MPW             0x1c0       /* base 0x1cc; V */
#define NS_MPW_START           0x00927720  /* base 00962750; V re-checked ([+0x180], [+0x1a8]; RET 4) */
#define NS_MPW_BACK            0x009276f0  /* base 00962720; V re-checked */
#define NS_MPW_SEND            0x00927860  /* base 00962890; V re-checked ([+0x1a0]) */
#define NS_MPW_SLOT_OF         0x00bc7ed0  /* base 00c98d20; V hash (lan-screens-hof.md) */
#define NS_MPW_SERVICES        0x00611ab0  /* base 005e8050; V */
#define NS_MPW_MATCHMAKER_ID   0x8c348b00  /* same; V */
#define NS_MPW_MM_GAME         0x44        /* same; V (IMatchMaker slots unchanged) */
#define NS_MPW_INFO_CTOR       0x0072fa60  /* base 0071bfa0; V */
#define NS_MPW_INFO_DTOR       0x00730270  /* base 0071d020; V */
#define NS_MPW_INFO_SIZE       0x158       /* base 0x128; V */
#define NS_MPW_HOST            0x1a4       /* base 0x1b0; V */
#define NS_MPW_LOGIC           0x1a8       /* base 0x1b4; V re-checked */
#define NS_MPW_CHAT            0x19c       /* base 0x1a8; V */
#define NS_MPW_EDIT            0x1a0       /* base 0x1ac; V re-checked */
#define NS_MPW_STARTING        0x180       /* base 0x18c; V re-checked */
#define NS_MPW_ML_ACCEPTING    0x100       /* base 0x108 (the enum ML_ACCEPTING); V host start 00984788 */
/* same as base (V): GI_TIME 0x38, GI_DIFFICULTY 0x3c, GI_GOAL 0xec, GI_NAME 0x1c, GI_MAX 0x6c, GI_SIZE 0x70,
 * GI_MAP 0xcc; logic +0x10 session, +0x18 status (box at +0x18), +0x34 host id, +0x38 rows, +0x60 got-id,
 * +0x74 model, CanStart vt+0x18; row +0x28 state, +0x44 colour, +0x60 race, +0x7c bonus, +0x80 hero,
 * +0xbc/+0xc0 request, +0xc4 locks, +0xf4 name (lan-screens-hof.md §2.6). */
#define NS_MPW_GI_PLAYERS      0x84        /* players in = [info+0x84], as HoF's own list reads it; V */
#define NS_MPO_QUICK_LABEL     "Быстрая схватка", "Quick combat"   /* HoF's own words; V */
#define NS_MPO_AUTOSAVE_LABEL  "Динамичные бои", "Dynamic battles" /* `realtime` in HoF; V */
