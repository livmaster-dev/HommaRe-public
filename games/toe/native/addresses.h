/* games/toe/native/addresses.h - Tribes of the East's addresses for the
 * shared native code (native/touch.c, save.cpp, camera.c, horplus.c, ui_bounds.c),
 * which the build passes it to as RECOMP_ADDRESS_HEADER (game.toml
 * [translate] native addresses); the translated chunks never include it, so
 * changing one only rebuilds those sources. Found with games/map_addresses.py
 * from the base game's (native/touch.c): functions and data through
 * byte-identical functions, vtables through RTTI. */
#pragma once

#define HOMM5_CURSOR_OBJECT 0x0110c470 /* NUI::CCursor */
#define HOMM5_CURSOR_VTABLE 0x00f56878
#define HOMM5_CURSOR_X 0x0110c464
#define HOMM5_CURSOR_Y 0x0110c468
#define HOMM5_CURSOR_ID 0x0110c4b8
/* CCursor::Draw and its hardware-cursor flag (getter 0060e0c0). */
#define HOMM5_FN_CURSOR_DRAW fn_0060da80
#define HOMM5_CURSOR_HW 0x0110c450
#define HOMM5_HOVER_TIMEOUT 0x0108e21c /* Options.Tooltips.HoverTimeout, 1.0 */
#define HOMM5_SCREEN_STACK 0x0110b6ec
/* CAdventureScreen grew by 0x90 and CCombatArenaScreen shrank by 0xf4 since
 * the base game; their IScreen subobjects keep their place in the class. */
#define HOMM5_VT_ADVENTURE 0x00f5d0a4
#define HOMM5_VT_COMBAT 0x00f592a4
#define HOMM5_SUB_ADVENTURE 0x844
#define HOMM5_SUB_COMBAT 0x564
#define HOMM5_SCREEN_WINDOWS 0xa0
#define HOMM5_WINDOWS_MODAL 0x1f4
#define HOMM5_SAVE_SETTINGS 0x005c5830 /* the base game's 006bfe80 */

/* The adventure map's camera (native/camera.c): what CCameraOperator::Update
 * (006fd1e0, the base game's 007f26b0) reaches, mapped as above; the screen
 * fields from the adventure screen's init. */
#define HOMM5_CAM_VTABLE 0x00f62010       /* CCamera */
#define HOMM5_CAM_OPERATOR_VTABLE 0x00f62450 /* CCameraOperator */
#define HOMM5_SCREEN_CAMERA 0xa4
#define HOMM5_SCREEN_OPERATOR 0x5e4
#define HOMM5_VIRTUAL_SCREEN 0x01093780 /* the UI's virtual screen size, floats */
#define HOMM5_CAMERA_UPDATE_ORIG fn_006fd1e0

/* The battle camera's start (native/camera.c): CArenaCameraConfigurator
 * (vtable 0xf61ea0) slot 8, found through its RTTI. */
#define HOMM5_BATTLE_DEFAULTS_ORIG fn_006f39d0
/* The battle screen's camera operator (native/camera.c zoom_view_camera): the
 * battle's camera setup 0064c0d0 repeats the base game's 0074bcd0 call for
 * call and keeps the operator, made by CCameraOperator's constructor 006fb760
 * (found by its vtable 0xf62450), here; the base game's +0x4e4. */
#define HOMM5_COMBAT_OPERATOR 0x3b8

/* Hor+ (native/horplus.c): the base game's 00a1a1e0 (projection), 00a18c90
 * (screen -> ray), 00a19030 (world -> screen) and 00a19940 (the engine's
 * wide-screen angle), byte-identical here. Tribes of the East widens the
 * interface's virtual screen and anchors its screens itself (SetScreenSize
 * 00e31fa0, data in a2p1-data.pak), so it needs only these. */
#define HOMM5_FN_PROJECTION fn_00966970
#define HOMM5_FN_SCREEN_RAY fn_00965420
#define HOMM5_FN_WORLD_TO_SCREEN fn_009657c0
#define HOMM5_FN_FOV_FOR_SCREEN fn_009660d0

/* Interface bounds (native/ui_bounds.c): the base game's axis 00e43650 is
 * 00e3d540 here, asked for x from SetParentRect 00e35620 at 00e3572e; the
 * current virtual screen is 01093780 (00e31fa0 widens it). */
#define HOMM5_VIRTUAL_W 0x01093780
#define HOMM5_FN_ANCHOR_AXIS fn_00e3d540
#define HOMM5_RET_ANCHOR_X 0x00e35733
#define HOMM5_RET_ANCHOR_Y 0x00e3577d /* and for y (the dragged icon, native/ui_bounds.c; the phone rows, native/phone_layout.c) */
#define HOMM5_FN_COUNTER_MAKE fn_00739b00 /* a battle's stack counter made (the base game's 008333f0) */
#define HOMM5_FN_SET_PLACEMENT fn_00e35920 /* a window's SetPlacement (the base game's 00e3b9f0) */

/* The phone's interface (native/phone_hud.c), from the base game's through
 * byte-identical functions, RTTI and the classes' sub-object offsets
 * (research notes phone-hud-expansions-addresses.md, 2026-10-19). The root
 * placement 00e734a0 is the base game's 00e74f30 byte for byte, the alignment
 * fields of its description included. "probable": the town's, hire, build,
 * hero and loading screens' IScreen vtables and offsets (by structure), the
 * clipped draw 00e3f9c0, the loading screen's constructor 0069ebe0 and the
 * tooltip timer 00e9b230; the rest hash-identical. */
#define HOMM5_VT_TOWN 0x00f734d4 /* NUI::CTownScreen's IScreen, at +0x4c0 */
#define HOMM5_SUB_TOWN 0x4c0
#define HOMM5_VT_LOADING 0x00f5c7d8 /* NUI::CPWLScreenInterface's, at +0x1e4 */
#define HOMM5_SUB_LOADING 0x1e4
#define HOMM5_VT_HIRE 0x00f72c10 /* NUI::CHireScreen's, at +0x290 */
#define HOMM5_SUB_HIRE 0x290
#define HOMM5_VT_BUILD 0x00f74fe4 /* NUI::CTownBuildScreen's, at +0x26c */
#define HOMM5_SUB_BUILD 0x26c
#define HOMM5_VT_HERO 0x00f665c0 /* NUI::CHeroScreen2's, at +0x844 */
#define HOMM5_SUB_HERO 0x844
#define HOMM5_VT_LOADING_MAIN 0x00f5c770 /* NUI::CPWLScreenInterface */
#define HOMM5_VT_MS_BUTTON 0x00fec640
#define HOMM5_VT_EDIT_LINE 0x00fed64c
#define HOMM5_VT_EDIT_BOX 0x00ff6010
#define HOMM5_VT_LIST 0x00fecbcc
#define HOMM5_VT_SCROLLER 0x00ff15bc
#define HOMM5_VT_TEXT_VIEW 0x00ff2004
#define HOMM5_UI_VIRTUAL_W 0x01093780
#define HOMM5_UI_VIRTUAL_H 0x01093784
#define HOMM5_UI_REAL_W 0x01093790
#define HOMM5_UI_REAL_H 0x01093794
#define HOMM5_UI_ORIGIN_X 0x0121ae48
#define HOMM5_UI_ORIGIN_Y 0x0121ae4c
#define HOMM5_FN_WINDOW_DRAW fn_00e37c10
#define HOMM5_FN_WINDOW_CONTAINS fn_00e35c30
#define HOMM5_FN_SIMPLE_DRAW fn_00e3f9c0
#define HOMM5_FN_TEXT_LAYOUT fn_00e3f2e0
#define HOMM5_ADDR_TEXT_LAYOUT 0x00e3f2e0u
#define HOMM5_FN_LOADING_SCREEN fn_0069ebe0
#define HOMM5_FN_SCREEN_MOUSE_0 fn_00e368e0
#define HOMM5_FN_SCREEN_MOUSE_1 fn_00e369a0
#define HOMM5_FN_SCREEN_MOUSE_2 fn_00e36a30
#define HOMM5_FN_SCREEN_MOUSE_3 fn_00e36af0
#define HOMM5_FN_SCREEN_MOUSE_4 fn_00e36bb0
#define HOMM5_FN_SCREEN_MOUSE_5 fn_00e36c40
#define HOMM5_FN_SCREEN_MOUSE_6 fn_00e36d00
#define HOMM5_FN_TOOLTIP_TIMER fn_00e9b230
#define HOMM5_FN_TOOLTIP_MOVE fn_00e9b170
/* The game aligns its roots itself (its data's HorAlign/VerAlign tags):
 * native/phone_hud.c's own placement hook only moves them into the safe area. */
#define HOMM5_FN_SCREEN_PLACE fn_00e734a0
#define HOMM5_ADDR_SCREEN_PLACE 0x00e734a0u
#define HOMM5_HUD_PLACE_HOOK 1
/* CSkeletonAnimator's NeedUpdate, the translated original (base 0067c060). */
#define HOMM5_ANIM_NEED_UPDATE_ORIG fn_005640e0

/* granny2.dll (this game's own build of it): the quadratic B-spline
 * quaternion sample 50011890 and the float constants it reads
 * (native/granny.c). */
void fn_50011890(X86 *c);
#define GRANNY_SPLINE_QUAT_ORIG fn_50011890
#define GRANNY_SPLINE_ONE 0x5004c0d0u
#define GRANNY_SPLINE_NORM_A 0x5004c478u
#define GRANNY_SPLINE_NORM_B 0x5004c1d0u
/* granny2.dll 500127b0: the knot interval search before each curve sample. */
void fn_500127b0(X86 *c);
#define GRANNY_KNOT_SEARCH_ORIG fn_500127b0
/* granny2.dll 50028950: one weighted local transform into the blend
 * accumulator, and the orientation threshold it compares with. */
void fn_50028950(X86 *c);
#define GRANNY_POSE_ACCUMULATE_ORIG fn_50028950
#define GRANNY_POSE_DOT_MIN 0x5004c0ccu
/* granny2.dll 50028b90: finish the blended local pose of a run of bones, and
 * the constants of its orientation normalisation. */
void fn_50028b90(X86 *c);
#define GRANNY_POSE_FINISH_ORIG fn_50028b90
#define GRANNY_NORM_HI 0x5004c6d8u
#define GRANNY_NORM_LO 0x5004c6d4u
#define GRANNY_NORM_POLY_A 0x5004c6d0u
#define GRANNY_NORM_POLY_B 0x5004c6ccu
/* The MMX vertex normal skinning (native/lighting.c): 00557470 and its reciprocal
 * square root table. */
void fn_00557470(X86 *c);
#define HOMM5_LIGHT_VERTEX_ORIG fn_00557470
#define HOMM5_LIGHT_RSQRT 0x010f94b0u
/* Skinned parts drawn on the GPU (native/gpu_skin.cpp): the part fill
 * 00556660, the staged copy 005559c0 and the big vertex buffer's allocation
 * 00500400 (the base game's 0066e4d0, 0066d950 and 00606af0), 0068fc20's
 * alpha (00587570) and its two global vectors (01138ee8 / 01138ef4 there),
 * the reciprocal square root and cube-root tables (01139088 / 01142088). */
#define H5SKIN_FILL_ORIG fn_00556660
#define H5SKIN_COPY_ORIG fn_005559c0
#define H5SKIN_ALLOC_ORIG fn_00500400
#define H5SKIN_ALPHA_FN 0x00587570u
#define H5SKIN_ALPHA_A 0x010f9300u
#define H5SKIN_ALPHA_B 0x010f930cu
#define H5SKIN_RSQRT 0x010f94b0u
#define H5SKIN_CBRT 0x011024b0u
#define H5SKIN_LOW_RAM 0x0108d1e8u /* gfx_low_ram's byte (the base game's 010cd1f9) */
#define H5SKIN_POSITIONS_ORIG fn_005578e0 /* the bounds' SSE position skinning (0066f600) */


/* native/overrides.c: the x87 float max/min helpers, byte for byte the base
 * game's 00614540 / 00614570; the base game's box grow 0066e070 has no
 * counterpart here (HOMM5_AABB_ADD_ORIG left undefined). */
#define HOMM5_FLOAT_MAX_ORIG fn_0050e5b0
#define HOMM5_FLOAT_MIN_ORIG fn_0050e5e0

/* native/scene.c: the 32-bit index buffer's fill (the base game's 0060d260)
 * and its "no base" flag (0x011375e8 there). */
#define HOMM5_IB_FILL_ORIG fn_00507290
#define HOMM5_IB_NOBASE 0x010f7898u

/* native/scene.c: the cull tests (the base game's 00a1a680, 00a1a9f0, 00a1ab50)
 * and the sign mask they XOR with (four 0x80000000). */
#define HOMM5_CULL_TEST_ORIG fn_00966e10
#define HOMM5_CULL_PUSH_ORIG fn_00967180
#define HOMM5_CULL_OBJECT_ORIG fn_009672e0
#define HOMM5_CULL_SIGNMASK 0x00f4a240u

/* native/scene.c: a vertex pool's range (the base game's 0060d810). */
#define HOMM5_POOL_ALLOC_ORIG fn_00507840

/* The end of a frame's 3D world (native/render_split.c): CGameView::Draw
 * (the NGScene::CGameView vtable's slot 0x50) and where the world pass gets
 * back from it - its main, second and alternate view. */
#define HOMM5_FN_GAME_VIEW_DRAW fn_00519620
#define HOMM5_RET_VIEW_MAIN 0x006cc05eu
#define HOMM5_RET_VIEW_SECOND 0x006cc08au
#define HOMM5_RET_VIEW_ALT 0x006cbd7fu

/* Native screens (native/native_screens.c, screens_*.c): research notes phone-menus/native/
 * expansion-toe-addresses.md. */
/* ---- native/native_screens.c ---- */
#define NS_SCREEN_STACK 0x0110b6ec   /* V votes 39 (= HOMM5_SCREEN_STACK) */
#define NS_DIFFICULTY   0x00abe040   /* V hash (base 00b97bd0) */
#define NS_TEXT         0x00956620   /* V hash (base 00a09de0) */
#define NS_FREE         0x004dd510   /* V votes 14084 (base 005e40c0) */
/* NEW macros native_screens.c needs (base value in the comment): */
#define NS_IW_FIND      0x94         /* V base 0x8c: 00e3c3b0 ≙ 00e362e0 at IWindow(+0x160) slot 0x94 */
#define NS_IW_CAPTION   0xc0         /* V base 0xb4: 00e3d610 ≙ 00e37540 (ratio .98) */
/* WINDOW_PART 0x10c, WINDOW_PARENT 0x28, WINDOW_CHILDREN 0xa4, ROOT_WINDOW 0xf8,
 * NS_SCREEN_ROOT 0xa0, NS_ROOT_DRAW 0x14: unchanged (phone-hud-expansions-addresses.md §3, V). */

/* ---- native/screens_question.c + overrides ---- */
#define NS_VT_MSGBOX      0x00f604f4 /* V RTTI CCommonMsgBox, 13 slots agree */
#define NS_VT_MSGBOX_TEXT 0x00f697a4 /* V RTTI CCommonMsgBoxWithUpdatableText */
#define NS_MSGBOX_SETUP_ORIG fn_006d32e0 /* V base 007c9d90; same args, RET 0x18 (diff: fields +4) */
/* games/toe/native/overrides.h:  void homm5_msgbox_setup(X86 *c); #define FN_006d32e0 homm5_msgbox_setup
 * (screens_question.c calls fn_007c9d90 by name: needs this macro). */

/* ---- native/screens_custom.c ---- */
#define NS_VT_CREATE       0x00f7bc30 /* V RTTI, last subobject */
#define NS_SUB_CREATE      0x1cc      /* V (base 0x1c4) */
#define NS_VT_WAIT         0x00f7f330 /* V RTTI */
#define NS_SUB_WAIT        0x2f8      /* V (base 0x2c4) */
#define NS_NEW             0x004dd500 /* V votes 8645 (jmp 004dd2d0) */
#define NS_CREATE_REACTION 0x008ea120 /* V +0x174 vtable slot 0; adds rmg_filter_changed, custom_game_show_rmg_maps */
#define NS_CREATE_REFRESH  0x008ead40 /* V diff .983 (only IWindow/ISlider slots) */
#define NS_MAPS_SORT       0x00915ab0 /* V diff .979 */
#define NS_MAPS_SELECTED   0x00917980 /* V diff (entry row 0x84->0x90) */
#define NS_MAP_NAME        0x00914d70 /* V 9 votes; tag field 0x54->0x4c inside */
#define NS_MAP_DESC        0x00914b50 /* V 2 votes; tag field 0x60->0x58 inside */
#define NS_MAP_GOAL        0x00914c00 /* V 5 votes. CHANGED: fastcall ECX = out wstr (built by it), EDX = info+0x6c; returns ECX; caller frees out.begin (as NS_MAP_SIZE) */
#define NS_MAP_PLAYERS     0x00917d40 /* V hash */
#define NS_MAP_SIZE        0x00629e30 /* V hash */
#define NS_MP_MAPS         0x01118d1c /* V 008eaec0 */
#define NS_SLIDER_VT       0x00ff180c /* V RTTI CWindowSlider +0xf8 */
#define NS_WAIT_REACTION   0x00933110 /* V slot 0, diff .98 */
#define NS_VT_HS_CREATE    0x00f7bdd8 /* V RTTI */
#define NS_SUB_HS_CREATE   0x198      /* V (base 0x194) */
#define NS_VT_HS_WAIT      0x00f7f4b4 /* V RTTI */
#define NS_SUB_HS_WAIT     0x1bc      /* V same */
#define NS_HS_CREATE       0x008ec110 /* V registration in 008ebe20 ("create", len 6) + diff .967 */
#define NS_HS_LOAD         0x008ec4b0 /* V hash */
#define NS_HS_BACK         0x008ec540 /* V registration (after load); ToE also sets a flag on the screen below */
#define NS_HS_MAPSELECTED  0x008ec5a0 /* V identical body */
#define NS_HS_START        0x00936280 /* V hash + registration in 00935e40 (hash of 009f18f0) */
#define NS_HS_WAIT_BACK    0x00936260 /* V registration in 00935e40 */
#define NS_HS_NAME_ACCEPT  0x00936300 /* V registration in 00935e40 */
#define NS_RESOURCE_OF     0x00984690 /* V hash */
#define NS_DYNAMIC_CAST    0x0094ab92 /* V votes 897; same call shape in 009264a0 */
#define NS_RTTI_RESOURCE   0x010a8110 /* V ".?AVCResource@NDb@@" */
#define NS_RTTI_HERO_SHARED 0x010aa044 /* V ".?AUSAdvMapHeroShared@NDb@@" */
#define NS_TEXT_REF        0x00956620 /* V */

/* ---- native/screens_duel.c ---- */
#define NS_VT_DUEL         0x00f7bfc0 /* V RTTI CHSDMWaitScreen<CHeroInfoView> */
#define NS_VT_DUEL3        0x00f7bf18 /* V RTTI CHSDMWaitScreen<CTripleHeroesView> */
#define NS_SUB_DUEL        0x1d4      /* V (base 0x1d0) */
#define NS_DUEL_NEXT       0x008ef3e0 /* V ctor 008edb20 registration, body identical */
#define NS_DUEL_BACK       0x008ef340 /* V same */
#define NS_DUEL3_NEXT      0x008ef430 /* V ctor 008eddd0 registration, body identical */
#define NS_DUEL3_BACK      0x008ef390 /* V same */
#define NS_DUEL3_PUT       0x00931990 /* V 4 votes, diff (block 0xe4->0xe8) */
#define NS_DUEL3_VIEW      0x00931b00 /* V 4 votes */
#define NS_DUEL3_MOVER     0x00931e30 /* V 2 votes */
#define NS_DUEL_ARENAS     0x009264a0 /* V identical body */
#define NS_DUEL_ARENAS_OF  0x01119b68 /* V 3 votes (MOV ECX,0x1119b68 in 00925d50/00926a10) */
#define NS_DUEL_SPEED_NAME 0x00abe110 /* V 6 votes */
#define NS_TEXT_OF_REF     0x00956620

/* ---- native/screens_escmenu.c ---- */
#define NS_VT_ESC_MENU     0x00f64be8 /* V RTTI */
#define NS_SUB_ESC_MENU    0x1e0      /* V (base 0x1dc) */
#define NS_ESC_REACTION    0x007341e0 /* V +0x174 vtable slot 0, diff .977 */

/* ---- native/screens_hof.c ---- */
#define NS_VT_HOF          0x00f76308 /* V RTTI */
#define NS_SUB_HOF         0x1cc      /* V same */
#define NS_HOF_REACTION    0x008879a0 /* V slot 0, diff .99 */
#define NS_HOF_PICK        0x008880b0 /* V 4 votes, diff .99 */
#define NS_HOF_CLEAR       0x00888ac0 /* V hash */
#define NS_HOF_CLEAR_OK    0x17363b00 /* V same immediate in 008879a0/00888ac0 */

/* ---- native/screens_load.c ---- */
#define NS_VT_LOAD         0x00f575fc /* V RTTI */
#define NS_SUB_LOAD        0x2b4      /* V same */
#define NS_LOAD_CLICK      0x00648fc0 /* V hash */
#define NS_LOAD_LOAD       0x0062ef20 /* V hash */
#define NS_LOAD_CONFIRMED  0x0062f0b0 /* V hash */
#define NS_LOAD_CLOSE      0x00644510 /* V 14 votes, identical body */
#define NS_LOAD_TYPE       0x00629930 /* V hash */
#define NS_LOAD_SIZE       0x00629e30 /* V hash */
#define NS_DIFF_OF_SAVE    0x00abe080 /* V hash */
#define NS_DIFF_ENTRY      0x00abe040 /* V hash */

/* ---- native/screens_menu.c ---- */
#define NS_VT_MENU         0x00f6b238 /* V RTTI */
#define NS_SUB_MENU        0x210      /* V (base 0x214) */
#define NS_MENU_REACTION   0x007b74e0 /* V hash */
#define NS_MENU_SWITCH     0x007b7900 /* V hash + all 13 jump-table cases identical */
#define NS_MENU_CREDITS    0x00893710 /* V identical body, 1 vote */
#define NS_MENU_EXIT       0x00738950 /* V hash */
#define NS_PROFILE_VALID   0x0088b690 /* V diff (passes game id 3 internally; same interface) */
#define NS_PROFILE_NAME    0x0088b6e0 /* V same */
#define NS_PROFILE_SHOWN   0x0088b670 /* V hash */
#define NS_PROFILE_SHOW    0x0088b5b0 /* V hash */
#define NS_PROFILE_DELETE  0x0088a7b0 /* V hash */
#define NS_SETUP_REACTION  0x008967f0 /* V hash */
#define NS_DIFFICULTY_NOW  0x01116814 /* V 7 votes */
#define NS_CAMPAIGN_OF     0x0088fd70 /* V hash */
#define NS_VT_EDIT_BOX     0x00f7ba44 /* V RTTI CSimpleEditBox */
#define NS_SUB_EDIT_BOX    0x1bc      /* V (base 0x1b8) */
#define NS_VT_FIRST_PROFILE 0x00f765c8 /* V RTTI CSelectFirstProfileNameScreen +0x174 */
#define NS_EDIT_BOX_OK     0x008e8840 /* V hash */

/* ---- native/screens_options.c ---- */
#define NS_VT_OPTIONS      0x00f69400 /* V RTTI */
#define NS_SUB_OPTIONS     0x1f8      /* V same */
#define NS_OPT_SHOW        0x0079fbc0 /* V hash */
#define NS_OPT_CHANGED     0x0079ed50 /* V hash */
#define NS_OPT_ACCEPT      0x0079f840 /* V hash */
#define NS_OPT_PAGE_BACK   0x0079fb10 /* V hash */
#define NS_OPT_CLOSE       0x0079fe30 /* V hash */
#define NS_OPT_DEFAULT     0x0079ff30 /* V hash */
#define NS_OPT_DEFAULT_OK  0x172a6c00 /* V same immediate */
#define NS_OPT_REVERT      0x0079e9c0 /* V hash */
#define NS_VT_OPT_HEADER   0x00f69474 /* V RTTI, 10 slots agree */
#define NS_VT_OPT_CHECK    0x00f694a4 /* V 9 slots */
#define NS_VT_OPT_COMBO    0x00f694d4 /* V 7 slots */
#define NS_VT_OPT_SLIDER   0x00f69504 /* V 6 slots */

/* ---- fields and slots that differ from the base game (§2) ---- */
#define NS_SLIDER_SET_SEGMENT 0x28
#define NS_SLIDER_GET_SEGMENT 0x2c
#define NS_E_ROW 0x90
#define NS_E_KIND 0x94
#define NS_W_TIME 0x214
#define NS_W_ROWS 0x224
#define NS_W_CONTROLLER 0x230
#define NS_W_MODEL 0x234
#define NS_W_DIFFICULTY 0x2b4
#define NS_HL_TIME 0x98
#define NS_HL_DIFFICULTY 0x9c
#define NS_HL_MODEL 0x114
#define NS_HS_SPEC 0x10c
#define NS_HS_NAME 0x16c
#define NS_HS_FACE_SMALL 0x134
#define NS_HS_FACE_BIG 0x12c
#define NS_MAP_GOAL_OUT 1          /* the goal formatter builds an out string (ECX out, EDX goal) */
#define NS_RACE_FIRST 3            /* Haven 3 .. Inferno 8, Fortress 9, Stronghold 10 */
#define NS_RACE_RANDOM 1
#define NS_NO_COMPUTER_RULE 1      /* who plays: no "computer allowed" byte */
#define NS_P_GRID 0x48
#define NS_P_SET_SETTINGS 0x38
#define NS_P_GET_SETTINGS 0x3c
#define NS_DUEL_SETTINGS_SIZE 0x18 /* +8 town type (new), speed at +0x14 */
#define NS_S_SPEED 0x14
#define NS_DUEL_INDEX 0xf0
#define NS_TRIPLE_SLOT_SIZE 0xe8
#define NS_TRIPLE_INDEX 0xe4
#define NS_B_YESNO 0x138
/* One loading picture per campaign (A2C0..A2C3 -> Pwl_C0..C3). */
#define NS_MISSION_PICTURE(out, cap, key, i) \
    snprintf(out, cap, "/UI/H5A2/PWL/Pwl_%s.(Texture).dds", (key)[0] && (key)[1] ? (key) + 2 : (key))
#define NS_GAME_VERSION "3.1"
#define NS_PAK_DIRS "data" /* the paks it mounts, for the native views' pictures */

/* The GS login (native/gs_login.c): the SDK's login entry, reached from
 * ProcessLogin (00e0e0d0) through the thunk 00419530, and
 * ProcessLoginRouterResult. */
#define HOMM5_FN_GS_LOGIN fn_00419c20
#define HOMM5_FN_GS_LOGIN_RESULT fn_00e0e500

/* ---- ubi.com screens (native/screens_ubi.c): Tribes of the East 3.1 ----
 * research notes lan/ubi-screens-expansions.md. Inner offsets = base (no NS_UBI_UL_* etc. needed). */
#define NS_UBI_VT_LOGIN         0x00f759b0  /* base 00fc2cb0; V RTTI CMPRegisterScreen IScreen */
#define NS_UBI_SUB_LOGIN        0x1dc       /* base 0x1e0; V (alloc 0x1e4 in 0087df60) */
#define NS_UBI_LOGIN_OK         0x0087c9e0  /* base 00947010; V reg "ok" in ctor 0087bd00, RET 4 */
#define NS_UBI_LOGIN_CANCEL     0x0087cdb0  /* base 009473e0; V reg "cancel"/"esc_pressed" → 0087bb90 */
#define NS_UBI_VT_EDIT_BOX      0x00f7ba44  /* base 00fc83e8; V (= NS_VT_EDIT_BOX) */
#define NS_UBI_SUB_EDIT_BOX     0x1bc       /* base 0x1b8; V (= NS_SUB_EDIT_BOX) */
#define NS_UBI_BOX_CALLBACK     0x00f75948  /* base 00fc2c48; V RTTI ISimpleEditBoxCallBack @0x17c; box built with ECX = screen+0x17c (0087c9a3) */
#define NS_UBI_BOX_OK           0x008e8840  /* base 009a7b80; V hash (lan-screens-toe.md) */
#define NS_UBI_BOX_CANCEL       0x008e8790  /* base 009a7ad0; V hash */
#define NS_UBI_VT_CHANNELS      0x00f68f4c  /* base 00fb70f0; V RTTI CMPChangeChannelScreen IScreen */
#define NS_UBI_SUB_CHANNELS     0x1e0       /* same; V (alloc 0x1e8 in 0079a6b0) */
#define NS_UBI_CHANNELS_JOIN    0x00798e80  /* base 0088e7a0; V reg "join" in ctor 007982f0, RET 4 */
#define NS_UBI_CHANNELS_CANCEL  0x00798e20  /* base 0088e740; V reg "cancel", RET 4 */
#define NS_UBI_VT_WAIT_BOX      0x00f5fed4  /* base 00fadd04; V RTTI CWaitBox */
#define NS_UBI_RTTI_IWINDOW     0x010aaf54  /* base 010e9ab0; V (IListControl vt+0x24 00e45720) */
#define NS_UBI_RTTI_LIST_ITEM   0x010ad650  /* base 010ed854; V */
#define NS_UBI_RTTI_OBJECT      0x010a79f8  /* base 010e67a0; V (join 00798f48) */
#define NS_UBI_RTTI_CHANNEL     0x010aed80  /* base 010ed774; V TD CChannelData (join 00798f43) */
#define NS_UBI_DYNAMIC_CAST     0x0094ab92  /* base 009fe372; V */
/* inner offsets: all equal base. V: login 0x1a8 / password 0x1ac / logging 0x1b0 / wait 0x1b4 / init 0x1b8 /
 * form 0x1bc (OK 0087c9e0, init 0087c190, init-start 0087ba00); box callback 0x17c / edit 0x180; channel list
 * 0x194 / ButtonJoin 0x1c0 / wait 0x1c4 (init 007985f0, join 00798e80); CChannelData 0x0c/0x18/0x24 (00799250);
 * wait box 0xf8 / 0x108 (006d3163, 006d6dd0); EDIT_TEXT 0x3c (0056c2b0); list 0x20/0x24/0x28 (00e45700/20/40). */

/* ---- lobbies in LAN and ubi.com mode + name box (native/screens_lan.c) ---- */
#define NS_LAN_VT_LOBBY         0x00f75334  /* base 00fc2180; V RTTI */
#define NS_LAN_SUB_LOBBY        0x214       /* base 0x200; V */
#define NS_LAN_VT_DUEL_LOBBY    0x00f75544  /* base 00fc237c; V */
#define NS_LAN_SUB_DUEL_LOBBY   0x20c       /* base 0x1f8; V */
#define NS_LAN_LOBBY_REACTION   0x00875200  /* base 0093b320; V re-checked: ECX = screen+0x174, RET 0xc, → 008d7650 */
#define NS_LAN_DUEL_LOBBY_REACTION 0x00876cc0 /* base 0093cda0; V re-checked */
#define NS_LAN_SELECTED         0x008dd120  /* base 0099a1a0; V hash */
#define NS_LAN_PLAYERS_NOW      0x008de890  /* base 0099b7f0; V re-checked: +0x180 < 1 → +0x84, else +0x184+0x188+0x18c */
#define NS_LAN_RTTI_IWINDOW     0x010aaf54  /* base 010e9ab0; V */
#define NS_LAN_RTTI_LIST_ITEM   0x010ad650  /* base 010ed854; V */
#define NS_LAN_RTTI_OBJECT      0x010a79f8  /* base 010e67a0; V */
#define NS_LAN_RTTI_GAME_DATA   0x010b25ac  /* base 010f0b6c; V */
#define NS_LAN_RTTI_PLAYER_DATA 0x010b3200  /* base 010f17d0; V */
#define NS_LAN_BOX_CALLBACK_STD 0x00f752dc  /* base 00fc2134; V */
#define NS_LAN_BOX_CALLBACK_DUEL 0x00f754f0 /* base 00fc2334; V */
#define NS_LAN_BOX_OK           0x008e8840  /* base 009a7b80; V */
#define NS_LAN_BOX_CANCEL       0x008e8790  /* base 009a7ad0; V */
#define NS_LAN_SPEED_NAME       0x00abe110  /* base 00b97ca0; V (= NS_DUEL_SPEED_NAME) */
#define NS_LAN_DYNAMIC_CAST     0x0094ab92  /* base 009fe372; V */
#define NS_LAN_TEXT             0x00956620  /* base 00a09de0; V (= NS_TEXT) */
#define NS_LAN_MAP_NAME         0x00914d70  /* base 009d9400; V (lan-screens-toe.md) fastcall ECX = info+0xcc */
#define NS_LAN_MAP_SIZE         0x00629e30  /* base 00729b30; V (= NS_MAP_SIZE) */
#define NS_LAN_SERVICES         0x004e1690  /* base 005e8050; V re-checked */
#define NS_LAN_MATCHMAKER_ID    0x8c348b00  /* same; V */
#define NS_LAN_CONTAINER_MODE   0x20        /* same; V re-checked (lobby init 008d8814) */
/* lobby fields (CLobbyScreenBase: +8 from the chat model on) */
#define NS_LAN_L_REACTIONS      0x174       /* same; V re-checked */
#define NS_LAN_L_CHAT           0x1a4       /* base 0x19c; V */
#define NS_LAN_L_PLAYERS        0x1ac       /* base 0x1a4; V re-checked */
#define NS_LAN_L_PROFILE        0x1b0       /* base 0x1a8; V re-checked (store 008d8a2d) */
#define NS_LAN_L_EDIT           0x1b4       /* base 0x1ac; V re-checked */
#define NS_LAN_L_LEAVING        0x1b8       /* base 0x1b0; V re-checked (channel vt+0x4c 008d7d30) */
#define NS_LAN_L_JOINING        0x1bc       /* base 0x1b4; V re-checked */
#define NS_LAN_L_JOIN           0x1c0       /* base 0x1b8; V re-checked */
#define NS_LAN_L_GAMES          0x1dc       /* base 0x1d4; V re-checked */
#define NS_LAN_GI_SPEED         0x178       /* base 0x10c; V (lan-screens-toe.md §2) */
/* same as base: PLAYERS_LIST 0x38, GV_LIST 0x08, GV_RUNNING 0x24, GV_OBSERVABLE 0x25, LIST_ROWS 0x20, LIST_SELECT 0x28,
 * ITEM_DATA 0x04, GAME_INFO 0x0c; GI_OPEN 0x18, GI_NAME 0x1c, GI_LOCKED 0x34 (= password non-empty), GI_MAX 0x6c,
 * GI_SIZE 0x70, GI_SAVED 0x90, GI_MAP 0xcc, GI_ARENA 0xf8; CHAT_* 0x0c/0x18/0x0c; PLAYER_NAME 0x0c; EDIT_TEXT 0x3c;
 * BOX_CB 0x17c, BOX_EDIT 0x180, BOX_ID 0x188. V (lan-screens-toe.md "Checked and equal"). */
#define NS_LAN_UBI_ONLY         1           /* the lobbies natively in ubi.com mode only */

/* ---- network game: create (ns_mp_create) and players (ns_mp_wait), Tribes of the East 3.1 ----
 * Screen fields = base; the options block differs. */
#define NS_VT_MPC              0x00f7b174  /* base 00fc797c; V RTTI CMPCreateScreen IScreen */
#define NS_SUB_MPC             0x1a4       /* base 0x1a0; V (alloc 0x1ac) */
#define NS_MPC_MAPSELECTED     0x008daf30  /* base 00997fe0; V re-checked (reg in 008db000, RET 4) */
#define NS_MPC_CREATE          0x008daa60  /* base 00997b30; V re-checked */
#define NS_MPC_LOAD            0x008daea0  /* base 00997f50; V re-checked (LAN load / ubi.com load by mode) */
#define NS_MPC_BACK            0x008daa30  /* base 00997b00; V re-checked */
#define NS_MPC_RMG             0x008daf60  /* new; V `rmg_screen`: [screen+0x18c] = 1, push CCreateRMGScreen (0091f6b0), RET 4 */
/* NS_MPC_MAPS 0x17c / NS_MPC_OPTIONS 0x184 / NS_MPC_CREATING 0x188: same as base; V re-checked */
/* CMPCreateOptions (init 00918c80, re-checked) */
#define NS_MPO_LOCKED          0           /* base 0x18 `lockedgame`: NONE (§3) */
#define NS_MPO_GHOST           0x18        /* base 0x1c; V */
#define NS_MPO_QUICK           0x1c        /* base 0x20; V `quick_combat` «Быстрая схватка» → info+0x36 */
#define NS_MPO_AUTOSAVE        0x20        /* base 0x24; V `realtime` «Автосохранение» */
#define NS_MPO_SIMULTANEOUS    0x3c        /* new; V `simultaneus` «Одновременные ходы» → info+0x16c */
#define NS_MPO_TIME            0x24        /* base 0x28; V */
#define NS_MPO_DIFFICULTY      0x28        /* base 0x2c; V */
#define NS_MPO_ANIMATION       0x34        /* base 0x38; V */
/* NS_MPO_NAME 0x10, NS_MPO_PASSWORD 0x14 (always enabled), NS_MPO_COMBAT 0x40: same; V */
#define NS_MPC_SPEED_NAME      0x00abe110  /* base 00b97ca0; V (= NS_DUEL_SPEED_NAME) */

#define NS_VT_MPW              0x00f6c91c  /* base 00fbaae0; V RTTI CMPWaitScreen IScreen */
#define NS_SUB_MPW             0x1cc       /* same; V */
#define NS_MPW_START           0x00899080  /* base 00962750; V re-checked ([+0x18c], [+0x1b4]; RET 4) */
#define NS_MPW_BACK            0x00899050  /* base 00962720; V re-checked */
#define NS_MPW_SEND            0x008991c0  /* base 00962890; V re-checked ([+0x1ac]) */
#define NS_MPW_SLOT_OF         0x00b8cdd0  /* base 00c98d20; V hash */
#define NS_MPW_SERVICES        0x004e1690  /* base 005e8050; V */
#define NS_MPW_MATCHMAKER_ID   0x8c348b00  /* same; V */
#define NS_MPW_MM_GAME         0x44        /* same; V */
#define NS_MPW_INFO_CTOR       0x0061c770  /* base 0071bfa0; V */
#define NS_MPW_INFO_DTOR       0x0061dba0  /* base 0071d020; V */
#define NS_MPW_INFO_SIZE       0x194       /* base 0x128; V */
#define NS_MPW_PLAYERS_NOW     0x008de890  /* base 0099b7f0; V (+0x180 max, +0x184..+0x18c counts) */
/* screen fields HOST 0x1b0, LOGIC 0x1b4, CHAT 0x1a8, EDIT 0x1ac, STARTING 0x18c, logic ACCEPTING 0x108,
 * GI_* above 0x100 untouched by the code, rows and logic: all same as base; V (start/back/send/host start
 * 008f3ff8 re-checked; rest lan-screens-toe.md). */
/* NS_MAP_GOAL is the out-string form here (NS_MAP_GOAL_OUT 1 already defined): see §3. */
#define NS_MPO_QUICK_LABEL     "Быстрая схватка", "Quick combat"   /* ToE's own words; V */
