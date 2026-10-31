/* games/toe/native/overrides.h - Tribes of the East's replacements for
 * translated functions: every generated chunk includes this before it defines
 * FN_<addr>, so keep the game's addresses for the native code in
 * native/addresses.h, which only those sources include. */
#pragma once

/* 0060cf20: the hover-tooltip delay getter (the base game's 0070cf80). */
void homm5_hover_timeout(X86 *c);
#define FN_0060cf20 homm5_hover_timeout

/* 0060da80: NUI::CCursor::Draw (the base game's 0070dae0); native/touch.c
 * draws the battle's action cursor over the hardware one. */
void homm5_cursor_draw(X86 *c);
#define FN_0060da80 homm5_cursor_draw

/* 006fd1e0: CCameraOperator::Update, the adventure map's camera (the base
 * game's 007f26b0); native/camera.c. */
void homm5_camera_update(X86 *c);
#define FN_006fd1e0 homm5_camera_update

/* 006f39d0: the battle camera's start (CArenaCameraConfigurator slot 8, the
 * base game's 007e9340); native/camera.c. */
void homm5_battle_camera(X86 *c);
#define FN_006f39d0 homm5_battle_camera

/* Hor+ (native/horplus.c): the base game's 00a1a1e0, 00a18c90, 00a19030 and
 * 00a19940, byte-identical here (addresses in native/addresses.h). */
void homm5_projection(X86 *c);
#define FN_00966970 homm5_projection
void homm5_screen_ray(X86 *c);
#define FN_00965420 homm5_screen_ray
void homm5_world_to_screen(X86 *c);
#define FN_009657c0 homm5_world_to_screen
void homm5_fov_for_screen(X86 *c);
#define FN_009660d0 homm5_fov_for_screen

/* Interface bounds (native/ui_bounds.c): the base game's axis 00e43650 is
 * 00e3d540 here (the rest in native/addresses.h). */
void homm5_anchor_axis(X86 *c);
#define FN_00e3d540 homm5_anchor_axis
/* 00739b00: a battle's stack counter made and added to the battle screen
 * (the base game's 008333f0). */
void homm5_counter_make(X86 *c);
#define FN_00739b00 homm5_counter_make
/* 00e35920: a window's SetPlacement (the base game's 00e3b9f0). */
void homm5_window_place(X86 *c);
#define FN_00e35920 homm5_window_place

/* The phone's interface (native/phone_hud.c; addresses and their sources in
 * native/addresses.h): the base game's 00e74f30 (root placement), 00e3dd70
 * (CWindow::Draw), 00e3bd00 (whether a point is a window's), 00e45ad0 (a
 * CWindowSimple's clipped draw), 00e453f0 (a text's layout), 0083be20 (the
 * loading screen's constructor), the WindowScreen mouse handlers 00e3c9b0 ..
 * 00e3cdd0 and the tooltip controller's 00ea0fc0 / 00ea0f00. */
void homm5_hud_screen_place(X86 *c);
#define FN_00e734a0 homm5_hud_screen_place
void homm5_window_draw(X86 *c);
#define FN_00e37c10 homm5_window_draw
void homm5_window_contains(X86 *c);
#define FN_00e35c30 homm5_window_contains
void homm5_simple_draw(X86 *c);
#define FN_00e3f9c0 homm5_simple_draw
void homm5_text_layout(X86 *c);
#define FN_00e3f2e0 homm5_text_layout
void homm5_loading_screen(X86 *c);
#define FN_0069ebe0 homm5_loading_screen
void homm5_screen_mouse_0(X86 *c);
#define FN_00e368e0 homm5_screen_mouse_0
void homm5_screen_mouse_1(X86 *c);
#define FN_00e369a0 homm5_screen_mouse_1
void homm5_screen_mouse_2(X86 *c);
#define FN_00e36a30 homm5_screen_mouse_2
void homm5_screen_mouse_3(X86 *c);
#define FN_00e36af0 homm5_screen_mouse_3
void homm5_screen_mouse_4(X86 *c);
#define FN_00e36bb0 homm5_screen_mouse_4
void homm5_screen_mouse_5(X86 *c);
#define FN_00e36c40 homm5_screen_mouse_5
void homm5_screen_mouse_6(X86 *c);
#define FN_00e36d00 homm5_screen_mouse_6
void homm5_tooltip_timer(X86 *c);
#define FN_00e9b230 homm5_tooltip_timer
void homm5_tooltip_move(X86 *c);
#define FN_00e9b170 homm5_tooltip_move

/* 005640e0: CSkeletonAnimator's NeedUpdate (the base game's 0067c060);
 * native/camera.c thins the animation of a far adventure-map camera. */
void homm5_anim_need_update(X86 *c);
#define FN_005640e0 homm5_anim_need_update

/* granny2.dll 50011890: the quadratic B-spline quaternion sample, natively
 * (native/granny.c; RECOMP_GRANNY_NATIVE=0 the original, check both). */
void granny_spline_quat(X86 *c);
#define FN_50011890 granny_spline_quat

/* granny2.dll 500127b0: the knot interval search, natively (native/granny.c). */
void granny_knot_search(X86 *c);
#define FN_500127b0 granny_knot_search

/* granny2.dll 50028950: the blend accumulator's add, natively (native/granny.c). */
void granny_pose_accumulate(X86 *c);
#define FN_50028950 granny_pose_accumulate

/* granny2.dll 50028b90: the blended local pose's finish, natively (native/granny.c). */
void granny_pose_finish(X86 *c);
#define FN_50028b90 granny_pose_finish

/* 00557470: one vertex normal skinned through one bone with MMX, natively
 * (native/lighting.c). */
void homm5_light_vertex(X86 *c);
#define FN_00557470 homm5_light_vertex

/* Skinned parts drawn from the bind pose on the GPU (native/gpu_skin.cpp,
 * RECOMP_HOMM5_GPU_SKIN): the part fill, the copy of a part's staged
 * vertices and the format 5 vertex buffer's allocation (the base game's
 * 0066e4d0, 0066d950, 00606af0). */
void homm5_skin_part_fill(X86 *c);
#define FN_00556660 homm5_skin_part_fill
void homm5_skin_copy(X86 *c);
#define FN_005559c0 homm5_skin_copy
void homm5_skin_alloc(X86 *c);
#define FN_00500400 homm5_skin_alloc

/* 005578e0: the bounds' SSE skinning of a part's positions (the base game's
 * 0066f600), in NEON (native/gpu_skin.cpp, RECOMP_HOMM5_NATIVE_BOUNDS). */
void homm5_skin_positions(X86 *c);
#define FN_005578e0 homm5_skin_positions


/* 0050e5b0 / 0050e5e0: the x87 float max/min helpers (the base game's
 * 00614540 / 00614570, byte for byte); native/overrides.c. */
void h5_float_max(X86 *c);
#define FN_0050e5b0 h5_float_max
void h5_float_min(X86 *c);
#define FN_0050e5e0 h5_float_min

/* 00507290: the dynamic 32-bit index buffer's fill (the base game's
 * 0060d260); native/scene.c. */
void homm5_ib_fill(X86 *c);
#define FN_00507290 homm5_ib_fill

/* 00966e10 / 00967180 / 009672e0: the cull tests, a sphere against the
 * culler's planes (the base game's 00a1a680 / 00a1a9f0 / 00a1ab50);
 * native/scene.c. */
void homm5_cull_test(X86 *c);
#define FN_00966e10 homm5_cull_test
void homm5_cull_push(X86 *c);
#define FN_00967180 homm5_cull_push
void homm5_cull_object(X86 *c);
#define FN_009672e0 homm5_cull_object

/* 00507840: a vertex pool's range (the base game's 0060d810), which opens
 * the range's pages for the write that follows; native/scene.c. */
void homm5_pool_alloc(X86 *c);
#define FN_00507840 homm5_pool_alloc

/* 00519620: CGameView::Draw. native/render_split.c marks the end of the frame's 3D
 * world for the host (host_render_scene_end). */
void homm5_game_view_draw(X86 *c);
#define FN_00519620 homm5_game_view_draw

/* 006d32e0 (base 007c9d90): a message box's setup, which every game question
 * passes through: native/screens_question.c notes its title and text. */
void homm5_msgbox_setup(X86 *c);
#define FN_006d32e0 homm5_msgbox_setup

/* 00443400: the GameSpy clDataList unbounded string reader (native/net_guard.c;
 * the base game's 004432d0). */
void homm5_gs_str_read(X86 *c);
#define FN_00443400 homm5_gs_str_read

/* 00419c20: the GS SDK's login and 00e0e500: ProcessLoginRouterResult (the
 * base game's 00419ae0 and 00b31380); native/gs_login.c. */
void homm5_gs_login(X86 *c);
#define FN_00419c20 homm5_gs_login
void homm5_gs_login_result(X86 *c);
#define FN_00e0e500 homm5_gs_login_result

/* 0064e160 (the battle's pick under the cursor), 0064e770 (the action for
 * it), 0064c360 (its left button release) and 0070d860 (the grid cells'
 * states): the base game's 0074de10, 0074e470, 0074bf70 and 0080bac0 for
 * native/tap_attack.c. */
void homm5_tap_pick(X86 *c);
#define FN_0064e160 homm5_tap_pick
void homm5_tap_action(X86 *c);
#define FN_0064e770 homm5_tap_action
void homm5_tap_click(X86 *c);
#define FN_0064c360 homm5_tap_click
void homm5_tap_grid(X86 *c);
#define FN_0070d860 homm5_tap_grid
