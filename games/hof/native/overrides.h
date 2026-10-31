/* games/hof/native/overrides.h - Hammers of Fate's replacements for
 * translated functions: every generated chunk includes this before it defines
 * FN_<addr>, so keep the game's addresses for the native code in
 * native/addresses.h, which only those sources include. */
#pragma once

/* 00726be0: the hover-tooltip delay getter (the base game's 0070cf80). */
void homm5_hover_timeout(X86 *c);
#define FN_00726be0 homm5_hover_timeout

/* 00727740: NUI::CCursor::Draw (the base game's 0070dae0); native/touch.c
 * draws the battle's action cursor over the hardware one. */
void homm5_cursor_draw(X86 *c);
#define FN_00727740 homm5_cursor_draw

/* 007df8b0: CCameraOperator::Update, the adventure map's camera (the base
 * game's 007f26b0); native/camera.c. */
void homm5_camera_update(X86 *c);
#define FN_007df8b0 homm5_camera_update

/* 007d5ee0: the battle camera's start (CArenaCameraConfigurator slot 8, the
 * base game's 007e9340); native/camera.c. */
void homm5_battle_camera(X86 *c);
#define FN_007d5ee0 homm5_battle_camera

/* The interface on wide screens (native/hof_screen.c): SetScreenSize gives the
 * coordinate conversions an effective 4:3 width and a centring origin; the
 * size getters and SetOrigin/GetOrigin keep the real values. */
void hof_set_screen_size(X86 *c);
#define FN_00e500d0 hof_set_screen_size
void hof_get_screen_size(X86 *c);
#define FN_00e500b0 hof_get_screen_size
void hof_screen_size_f(X86 *c);
#define FN_00e437c0 hof_screen_size_f
void hof_set_origin(X86 *c);
#define FN_00e50100 hof_set_origin
void hof_get_origin(X86 *c);
#define FN_00e50130 hof_get_origin

/* The 3D scene on wide screens (native/hof_screen.c): its camera gets the
 * screen's aspect from GetVirtualSize, and the pick and the world -> screen
 * projection work in that size with the interface's centring margin. */
void hof_virtual_size(X86 *c);
#define FN_00e437a0 hof_virtual_size
void hof_pick_ray(X86 *c);
#define FN_009feb50 hof_pick_ray
void hof_world_to_screen(X86 *c);
#define FN_009feed0 hof_world_to_screen

/* Hor+ (native/horplus.c) on the projection 009ffda0 (native/addresses.h). */
void homm5_projection(X86 *c);
#define FN_009ffda0 homm5_projection

/* Point conversions called with sizes (native/hof_screen.c size_sites):
 * the centring origin is left out for those call sites. */
void hof_point_to_screen(X86 *c);
#define FN_00e43900 hof_point_to_screen
void hof_point_to_screen2(X86 *c);
#define FN_00e439f0 hof_point_to_screen2
void hof_point_to_screen3(X86 *c);
#define FN_00e43ae0 hof_point_to_screen3
void hof_x_to_screen(X86 *c);
#define FN_00e43b30 hof_x_to_screen
void hof_x_from_screen(X86 *c);
#define FN_00e43b90 hof_x_from_screen
void hof_point_from_screen(X86 *c);
#define FN_00e43bf0 hof_point_from_screen
void hof_point_from_screen_i(X86 *c);
#define FN_00e43c40 hof_point_from_screen_i

/* The wide-screen pak (native/hof_paks.c): 009eded0 mounts one pak; a
 * zz-*.pak from the profile mounts above dataa1. */
void hof_mount_pak(X86 *c);
#define FN_009eded0 hof_mount_pak

/* Interface bounds and stretched roots (native/ui_bounds.c): the base game's
 * axis 00e43650 is 00e4eba0 here (the rest in native/addresses.h). */
void homm5_anchor_axis(X86 *c);
#define FN_00e4eba0 homm5_anchor_axis
/* 00e479c0: a window's SetPlacement (the base game's 00e3b9f0), for the
 * windows the game places itself (native/ui_bounds.c). */
void homm5_window_place(X86 *c);
#define FN_00e479c0 homm5_window_place

/* The adventure minimap's picture and mouse (native/hof_screen.c) at the
 * window's laid-out corner instead of its description's Position. */
void hof_minimap_draw(X86 *c);
#define FN_008333f0 hof_minimap_draw
void hof_minimap_button(X86 *c);
#define FN_00833760 hof_minimap_button
void hof_minimap_move(X86 *c);
#define FN_00833910 hof_minimap_move

/* A battle's stack counter placed over its creature (native/hof_screen.c):
 * the counter windows are noted, for native/ui_bounds.c. */
void hof_counter_place(X86 *c);
#define FN_007518e0 hof_counter_place

/* The phone's interface (native/phone_hud.c; addresses and their sources in
 * native/addresses.h): the base game's 00e3dd70 (CWindow::Draw), 00e3bd00
 * (whether a point is a window's), 00e45ad0 (a CWindowSimple's clipped draw),
 * 00e453f0 (a text's layout), 0083be20 (the loading screen's constructor),
 * and 00e7f630, where a root takes its description (no root placement here;
 * a root keeps the plain mapping, so its mouse handlers and tooltips are left
 * as they are). */
void homm5_window_draw(X86 *c);
#define FN_00e49cb0 homm5_window_draw
void homm5_window_contains(X86 *c);
#define FN_00e47cd0 homm5_window_contains
void homm5_simple_draw(X86 *c);
#define FN_00e51010 homm5_simple_draw
void homm5_text_layout(X86 *c);
#define FN_00e4f990 homm5_text_layout
void homm5_loading_screen(X86 *c);
#define FN_0082ef90 homm5_loading_screen
void homm5_hud_root_described(X86 *c);
#define FN_00e7f630 homm5_hud_root_described

/* 006a5a70: CSkeletonAnimator's NeedUpdate (the base game's 0067c060);
 * native/camera.c thins the animation of a far adventure-map camera. */
void homm5_anim_need_update(X86 *c);
#define FN_006a5a70 homm5_anim_need_update

/* 0063d930 / 0063d960: the x87 float max/min helpers (the base game's
 * 00614540 / 00614570, byte for byte); native/overrides.c. */
void h5_float_max(X86 *c);
#define FN_0063d930 h5_float_max
void h5_float_min(X86 *c);
#define FN_0063d960 h5_float_min

/* 00636690: the dynamic 32-bit index buffer's fill (the base game's
 * 0060d260); 00636c40: a vertex pool's range (0060d810), which opens the
 * range's pages for the write that follows; 00a00280 / 00a005f0 / 00a00750:
 * the cull tests (00a1a680 / 00a1a9f0 / 00a1ab50); native/scene.c. */
void homm5_ib_fill(X86 *c);
#define FN_00636690 homm5_ib_fill
void homm5_pool_alloc(X86 *c);
#define FN_00636c40 homm5_pool_alloc
void homm5_cull_test(X86 *c);
#define FN_00a00280 homm5_cull_test
void homm5_cull_push(X86 *c);
#define FN_00a005f0 homm5_cull_push
void homm5_cull_object(X86 *c);
#define FN_00a00750 homm5_cull_object

/* granny2.dll 50011890 / 500127b0 / 50028950 / 50028b90 natively
 * (native/granny.c; the same granny2.dll as the base game's). */
void granny_spline_quat(X86 *c);
#define FN_50011890 granny_spline_quat
void granny_knot_search(X86 *c);
#define FN_500127b0 granny_knot_search
void granny_pose_accumulate(X86 *c);
#define FN_50028950 granny_pose_accumulate
void granny_pose_finish(X86 *c);
#define FN_50028b90 granny_pose_finish

/* 00698d00: one vertex normal skinned through one bone with MMX (the base
 * game's 0066f2e0), natively (native/lighting.c). */
void homm5_light_vertex(X86 *c);
#define FN_00698d00 homm5_light_vertex

/* Skinned parts drawn from the bind pose on the GPU (native/gpu_skin.cpp):
 * the part fill, the staged copy, the format 5 allocation (the base game's
 * 0066e4d0, 0066d950, 00606af0) and the bounds' positions (0066f600). */
void homm5_skin_part_fill(X86 *c);
#define FN_00697ef0 homm5_skin_part_fill
void homm5_skin_copy(X86 *c);
#define FN_00697370 homm5_skin_copy
void homm5_skin_alloc(X86 *c);
#define FN_0062ffa0 homm5_skin_alloc
void homm5_skin_positions(X86 *c);
#define FN_00699020 homm5_skin_positions

/* 006565e0: CGameView::Draw. native/render_split.c marks the end of the frame's 3D
 * world for the host (host_render_scene_end). */
void homm5_game_view_draw(X86 *c);
#define FN_006565e0 homm5_game_view_draw

/* 007c3910 (base 007c9d90): a message box's setup, which every game question
 * passes through: native/screens_question.c notes its title and text. */
void homm5_msgbox_setup(X86 *c);
#define FN_007c3910 homm5_msgbox_setup

/* 00443240: the GameSpy clDataList unbounded string reader (native/net_guard.c;
 * the base game's 004432d0). */
void homm5_gs_str_read(X86 *c);
#define FN_00443240 homm5_gs_str_read

/* 00419a30: the GS SDK's login and 00e06e30: ProcessLoginRouterResult (the
 * base game's 00419ae0 and 00b31380); native/gs_login.c. */
void homm5_gs_login(X86 *c);
#define FN_00419a30 homm5_gs_login
void homm5_gs_login_result(X86 *c);
#define FN_00e06e30 homm5_gs_login_result
