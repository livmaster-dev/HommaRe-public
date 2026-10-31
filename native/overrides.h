/* native/overrides.h - Heroes V's native replacements for translated
 * functions (game.toml [translate] native). The generated sources include
 * this before they define FN_<addr>; a replacement is a #define here plus its
 * body in a source listed beside it. */
#pragma once

/* 00614540 / 00614570: hot x87 float max/min helpers; native/overrides.c. */
#define FN_00614540 h5_float_max
#define FN_00614570 h5_float_min
/* 0066e070: grow a mesh's bounding box by a vertex; native/overrides.c. */
#define FN_0066e070 h5_aabb_add
/* 0067c060: CSkeletonAnimator's NeedUpdate; native/camera.c thins the
 * animation of a far adventure-map camera to every other frame. */
void homm5_anim_need_update(X86 *c);
#define FN_0067c060 homm5_anim_need_update

/* 0070cf80: the hover-tooltip delay getter (Options.Tooltips.HoverTimeout in
 * ms), asked by the top screen every frame. native/touch.c answers from the
 * host's touch state instead, so a resting finger shows a tooltip at once and
 * a tap leaves none behind, without touching the variable user.cfg saves. */
void homm5_hover_timeout(X86 *c);
#define FN_0070cf80 homm5_hover_timeout

/* 0070dae0: NUI::CCursor::Draw. With the hardware cursor (the iOS default)
 * native/touch.c still draws the game's cursor in battle while it shows an
 * action, beside its damage hint. */
void homm5_cursor_draw(X86 *c);
#define FN_0070dae0 homm5_cursor_draw

/* 007f26b0: CCameraOperator::Update, the adventure-map camera's frame.
 * native/camera.c applies the touch gestures and camera buttons, then runs it. */
void homm5_camera_update(X86 *c);
#define FN_007f26b0 homm5_camera_update

/* 0062b9e0: CGameView::Draw, a screen's 3D world. native/render_split.c tells the
 * host when the world of the frame is drawn (host_render_scene_end), so the
 * interface after it draws at the screen's own resolution while the 3D was
 * drawn at the render quality's and upscaled. */
void homm5_game_view_draw(X86 *c);
#define FN_0062b9e0 homm5_game_view_draw

/* 00e38100: CVirtualScreenController::SetScreenSize, 00e74f30: a root
 * WindowScreen's placement on the virtual screen, and 00e43650: a child
 * window's position along an axis. native/widescreen.c widens the virtual
 * screen to the screen's aspect and centres untagged screens; native/
 * ui_bounds.c keeps edge-anchored windows in the player's interface bounds. */
void homm5_set_screen_size(X86 *c);
#define FN_00e38100 homm5_set_screen_size
void homm5_screen_place(X86 *c);
#define FN_00e74f30 homm5_screen_place
void homm5_anchor_axis(X86 *c);
#define FN_00e43650 homm5_anchor_axis
/* 008333f0: a battle's stack counter made and added to the battle screen;
 * native/ui_bounds.c notes it, so the interface bounds leave it where the
 * game puts it. */
void homm5_counter_make(X86 *c);
#define FN_008333f0 homm5_counter_make
/* 00e3b9f0: a window's SetPlacement (offsets set by code: a message box
 * centred, a dragged icon at the cursor); native/ui_bounds.c places such a
 * window from the screen's edge rather than the interface bounds'. */
void homm5_window_place(X86 *c);
#define FN_00e3b9f0 homm5_window_place

/* 00a1a1e0 (projection), 00a18c90 (screen -> ray), 00a19030 (world -> screen)
 * and 00a19940 (the engine's own wide-screen angle): native/horplus.c gives
 * wide screens the vertical view of a 4:3 one (Hor+). */
void homm5_projection(X86 *c);
#define FN_00a1a1e0 homm5_projection
void homm5_screen_ray(X86 *c);
#define FN_00a18c90 homm5_screen_ray
void homm5_world_to_screen(X86 *c);
#define FN_00a19030 homm5_world_to_screen
void homm5_fov_for_screen(X86 *c);
#define FN_00a19940 homm5_fov_for_screen

/* 007e9340: CArenaCameraConfigurator's battle camera start (vtable 0xfaffb4,
 * slot 8). native/camera.c brings it nearer on a phone. */
void homm5_battle_camera(X86 *c);
#define FN_007e9340 homm5_battle_camera

/* 00e3dd70 (CWindow::Draw), 00e45ad0 (a CWindowSimple's clipped draw),
 * 00e453f0 (a window text's layout),
 * 00e3bd00 (whether a point is a window's), 0083be20 (the loading screen's
 * constructor), the
 * WindowScreen mouse handlers 00e3c9b0, 00e3ca70, 00e3cb00, 00e3cbc0,
 * 00e3cc80, 00e3cd10, 00e3cdd0 and the tooltip controller's timer 00ea0fc0
 * and move 00ea0f00: native/widescreen.c keeps the interface inside a phone's
 * safe area and draws the map's and the battle's panels larger there. */
void homm5_window_draw(X86 *c);
#define FN_00e3dd70 homm5_window_draw
void homm5_window_contains(X86 *c);
#define FN_00e3bd00 homm5_window_contains
void homm5_simple_draw(X86 *c);
#define FN_00e45ad0 homm5_simple_draw
void homm5_text_layout(X86 *c);
#define FN_00e453f0 homm5_text_layout
void homm5_loading_screen(X86 *c);
#define FN_0083be20 homm5_loading_screen
void homm5_screen_mouse_0(X86 *c);
#define FN_00e3c9b0 homm5_screen_mouse_0
void homm5_screen_mouse_1(X86 *c);
#define FN_00e3ca70 homm5_screen_mouse_1
void homm5_screen_mouse_2(X86 *c);
#define FN_00e3cb00 homm5_screen_mouse_2
void homm5_screen_mouse_3(X86 *c);
#define FN_00e3cbc0 homm5_screen_mouse_3
void homm5_screen_mouse_4(X86 *c);
#define FN_00e3cc80 homm5_screen_mouse_4
void homm5_screen_mouse_5(X86 *c);
#define FN_00e3cd10 homm5_screen_mouse_5
void homm5_screen_mouse_6(X86 *c);
#define FN_00e3cdd0 homm5_screen_mouse_6
void homm5_tooltip_timer(X86 *c);
#define FN_00ea0fc0 homm5_tooltip_timer
void homm5_tooltip_move(X86 *c);
#define FN_00ea0f00 homm5_tooltip_move

/* 0060d260: the dynamic 32-bit index buffer's fill; 0060d810: a vertex
 * pool's range, which opens the range's pages for the write that follows;
 * 00a1a680 / 00a1a9f0 / 00a1ab50: the cull tests, a sphere against the
 * culler's planes; native/scene.c. */
void homm5_ib_fill(X86 *c);
#define FN_0060d260 homm5_ib_fill
void homm5_pool_alloc(X86 *c);
#define FN_0060d810 homm5_pool_alloc
void homm5_cull_test(X86 *c);
#define FN_00a1a680 homm5_cull_test
void homm5_cull_push(X86 *c);
#define FN_00a1a9f0 homm5_cull_push
void homm5_cull_object(X86 *c);
#define FN_00a1ab50 homm5_cull_object

/* granny2.dll 50011890 / 500127b0 / 50028950 / 50028b90: the spline
 * quaternion sample, the knot search, the blend accumulator's add and the
 * blended pose's finish, natively (native/granny.c; RECOMP_GRANNY_NATIVE). */
void granny_spline_quat(X86 *c);
#define FN_50011890 granny_spline_quat
void granny_knot_search(X86 *c);
#define FN_500127b0 granny_knot_search
void granny_pose_accumulate(X86 *c);
#define FN_50028950 granny_pose_accumulate
void granny_pose_finish(X86 *c);
#define FN_50028b90 granny_pose_finish

/* 0066f2e0: one vertex normal skinned through one bone with MMX, natively
 * (native/lighting.c). */
void homm5_light_vertex(X86 *c);
#define FN_0066f2e0 homm5_light_vertex

/* Skinned parts drawn from the bind pose on the GPU (native/gpu_skin.cpp,
 * RECOMP_HOMM5_GPU_SKIN): the part fill, the copy of a part's staged
 * vertices and the format 5 vertex buffer's allocation; 0066f600, the
 * bounds' SSE position skinning, in NEON (RECOMP_HOMM5_NATIVE_BOUNDS). */
void homm5_skin_part_fill(X86 *c);
#define FN_0066e4d0 homm5_skin_part_fill
void homm5_skin_copy(X86 *c);
#define FN_0066d950 homm5_skin_copy
void homm5_skin_alloc(X86 *c);
#define FN_00606af0 homm5_skin_alloc
void homm5_skin_positions(X86 *c);
#define FN_0066f600 homm5_skin_positions

/* 007c9d90: a message box's setup, which every game question passes
 * through: native/screens_question.c notes its title and text for the native
 * popup (RECOMP_NATIVE_SCREENS). */
void homm5_msgbox_setup(X86 *c);
#define FN_007c9d90 homm5_msgbox_setup

/* 0074de10 (the battle's pick under the cursor), 0074e470 (the action for
 * it), 0074bf70 (its left button release) and 0080bac0 (the grid cells'
 * states): native/tap_attack.c's "strike from a cell" mode,
 * RECOMP_TOUCH_BATTLE_CELLS=1, and the parking of a touch's cursor between
 * turns. */
void homm5_tap_pick(X86 *c);
#define FN_0074de10 homm5_tap_pick
void homm5_tap_action(X86 *c);
#define FN_0074e470 homm5_tap_action
void homm5_tap_click(X86 *c);
#define FN_0074bf70 homm5_tap_click
void homm5_tap_grid(X86 *c);
#define FN_0080bac0 homm5_tap_grid

/* 004432d0: the GameSpy clDataList unbounded string reader. native/net_guard.c
 * stands in for it and forwards every read, each into a fixed buffer, to the
 * bounded reader (00443390) with that buffer's size, so a hostile ubi-gs
 * server or peer cannot overflow a buffer with an over-long field (README
 * "Playing online safely"). */
void homm5_gs_str_read(X86 *c);
#define FN_004432d0 homm5_gs_str_read

/* 00419ae0: the GS SDK's login (name, password, version) and 00b31380:
 * NUbi::CStateWaitLoginRouterResult::ProcessLoginRouterResult. native/
 * gs_login.c sends a ubi-gs server a token, never the password. */
void homm5_gs_login(X86 *c);
#define FN_00419ae0 homm5_gs_login
void homm5_gs_login_result(X86 *c);
#define FN_00b31380 homm5_gs_login_result
