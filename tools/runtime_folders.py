import argparse
import re
import subprocess
import sys
from pathlib import Path

DESCRIPTION = ("The runtime's files by subsystem folder: which file of runtime/ moved from the top level into which "
               'folder, and the')
ROOT = Path(__file__).resolve().parents[1]
RT = ROOT / 'runtime'

FOLDERS = {
    'audio': [
        'audio_capture.c', 'audio_capture.h', 'audio_effects.c', 'audio_effects.h', 'audio_mixer.c', 'audio_reverb.c',
        'audio_reverb.h',
    ],
    'cfg': [
        'cfg.c', 'cfg_private.h', 'cfg_probe.c', 'cfg_support.c', 'hw_detect.c', 'hw_detect.h', 'preset_pick.h',
    ],
    'diag': [
        'bugcap.c', 'bugcap.h', 'debug_tools.c', 'debug_tools.h', 'dpi_probe_win.c', 'flight.c', 'flight.h',
        'flight_flash.h', 'floor_watch.c', 'floor_watch.h', 'hang_watch.c', 'mem_growth.h', 'perf.c', 'perf_bound.h',
        'perf_census.h', 'perfmon.c', 'perfmon.h', 'prof.h', 'prof_syms.c', 'prof_syms.h', 'prof_win.c', 'rdoc.c',
        'stutter_map.c', 'stutter_map.h', 'sysinfo.c', 'sysinfo.h', 'tracker_api.c',
    ],
    'gfx': [
        'astc_probe.c', 'gfx.h', 'gfx_abi_probe.c', 'gfx_guard.h', 'gfx_null.c', 'gfx_present.c', 'gfx_shader.c',
        'gfx_shader_guess.c', 'gfx_shader_probe.c', 'gfx_window_probe.c', 'gfx_window_sdl.c', 'gfx_window_win.c',
        'gfx_wsys.h', 'mesh_emu_spirv.c', 'mesh_emu_spirv.h', 'mesh_fb.c', 'mesh_fb.h', 'mesh_fb_hold.h',
        'mesh_fb_probe.c', 'mesh_pipeline_check.h', 'mfb_batch.h', 'prp_file.h', 'resolution.c', 'spirv_water.c',
        'spirv_water.h', 'spv_store.h', 'tex.h', 'tex_astc.c', 'tex_astc_mt.c', 'tex_deswizzle.c', 'tex_probe.c',
    ],
    'hle': [
        'amiibo.c', 'amiibo.h', 'amiibo_crypto.c', 'amiibo_crypto.h', 'audren.c', 'audren.h', 'fs.h', 'fs_host.c',
        'ipc.c', 'ipc.h', 'kernel.c', 'kernel.h', 'kernel_ap_stub.h', 'kernel_probe.c', 'movie.h', 'movie_probe.c',
        'net_offline.c', 'nfp.c', 'nfp.h', 'ov.h', 'ov_audio_input.c', 'ov_gfx.c', 'ov_input.c', 'ov_media.c',
        'ov_movie.c', 'ov_movie2.c', 'ov_probe.c', 'ov_system.c', 'ov_zstd.c', 'restbl.c', 'restbl.h', 'romfs.c',
        'romfs_path.h', 'sarc.c', 'sarc.h', 'save_share.c', 'save_share.h', 'save_zip.c', 'save_zip.h', 'svc.c',
        'swkbd.c', 'swkbd.h', 'swkbd_sdl.c', 'swkbd_win.c',
    ],
    'input': [
        'camera_speed.c', 'input.c', 'input.h', 'input_ds3.c', 'input_ds3.h', 'input_ds3_win.c', 'input_hd_pair.c',
        'input_hd_pair.h', 'input_map.c', 'input_map.h', 'input_sdl.c', 'input_sdl.h', 'input_win.c', 'input_win.h',
        'input_wsys.h', 'motion_fusion.c', 'motion_owner.c', 'motion_owner.h', 'mouse_direct.c', 'rumble_hd.c',
        'rumble_hd.h',
    ],
    'nvn': [
        'nvn.h', 'nvn_api.h', 'nvn_api_cmd.c', 'nvn_api_core.c', 'nvn_api_obj.c', 'nvn_ext.c', 'nvn_hmap.h',
        'nvn_obj.h', 'nvn_pool_index.h', 'nvn_probe.c', 'nvn_procs.c', 'nvn_procs.inc', 'nvn_submit_guard.h',
        'nvn_tex.c', 'nvn_tex.h', 'nvn_tex_probe.c',
    ],
    'pacing': [
        'dlssg_pace.h', 'dynres.c', 'frame_rate.c', 'rtss_decide.h', 'rtss_limit.c', 'vk_lowlat.c', 'vk_lowlat.h',
        'vk_present_held.h',
    ],
    'patches': [
        'action_prompt.c', 'action_prompt.h', 'amiibo_daily.c', 'amiibo_daily.h', 'animation_loop.c',
        'battle_music.c', 'boot_map.c', 'boot_map.h', 'boot_map_core.h', 'camera_clearance.c', 'camera_clearance.h',
        'changebank_hold.c', 'changebank_hold.h', 'cheats.c', 'cheats.h', 'heap_retry.c', 'heap_retry.h', 'mods.c',
        'mods.h', 'mods_probe.c', 'patches.c', 'sprint.c', 'sprint.h', 'text_speed.c', 'text_speed.h',
        'voice_language.c', 'wide_bars.c', 'wide_hud.c', 'wide_hud.h', 'widescreen.c', 'widescreen_math.h',
    ],
    'ui': [
        'folder_pick.h', 'folder_pick_sdl.c', 'folder_pick_win.cpp', 'menu_assets.c', 'menu_assets.h',
        'menu_cursor.c', 'menu_mouse.c', 'menu_mouse_geom.h', 'menu_sound.c', 'menu_sound.h', 'quit_menu.c',
        'quit_menu.h', 'quit_menu_core.h', 'quit_menu_text.c', 'quit_menu_text.h', 'ui_text.c', 'ui_text.h',
    ],
}
VK_EXTRA = ['boot_warm.h', 'fsr3_probe.c', 'fsr3fg_probe.c', 'gpu_choice.h', 'nvidia_cache.h', 'skin_pair.h', 'vk.h']
VK_ELSEWHERE = set(FOLDERS['pacing'])
FOLDERS['vk'] = sorted(VK_EXTRA + [
    'vk_aa.c', 'vk_astc.c', 'vk_backend.c', 'vk_build_gate.h', 'vk_cache_file.h', 'vk_cache_stamp.h', 'vk_caps.c',
    'vk_color.c', 'vk_contact.c', 'vk_crumbs.c', 'vk_device.c', 'vk_dlaa.c', 'vk_dlaa.h', 'vk_dlssg.c',
    'vk_dlssg.h', 'vk_dlssg_device.h', 'vk_dump.c', 'vk_evo_peek.h', 'vk_exec.c', 'vk_exec.h', 'vk_exec_draw.c',
    'vk_exec_probe.c', 'vk_exec_xfer.c', 'vk_ffx.cpp', 'vk_ffx.h', 'vk_ffx4.cpp', 'vk_ffx4_api.h', 'vk_fg.cpp',
    'vk_fg.h', 'vk_fg4.cpp', 'vk_fg_pose.inc', 'vk_fg_reset.h', 'vk_fg_stats.h', 'vk_fg_time.c', 'vk_fg_time.h',
    'vk_formats.c', 'vk_frame_owed.h', 'vk_fsr.c', 'vk_fsr3.c', 'vk_fsr3.h', 'vk_fsr3fg.c', 'vk_fsr3fg.h',
    'vk_gpa.c', 'vk_gpa.h', 'vk_hdr.c', 'vk_headroom.c', 'vk_index_rewrite.h', 'vk_mem_heap.h', 'vk_memory.c',
    'vk_mesh_fb.c', 'vk_mesh_fb.h', 'vk_mesh_path.h', 'vk_mvcheck.c', 'vk_mvprev.c', 'vk_mvsurvey.c',
    'vk_oq_hold.h', 'vk_pass_hooks.h', 'vk_pipeline.c', 'vk_play_gate.h', 'vk_rec.c', 'vk_rec.h',
    'vk_save_retry.h', 'vk_scratch_lru.h', 'vk_skin_motion.inc', 'vk_sync.c', 'vk_sync_hw.h', 'vk_target_frames.h',
    'vk_tex_compact.h', 'vk_tex_detail.h', 'vk_tex_release.h', 'vk_texture.c', 'vk_tonemap.c', 'vk_up_defer.h',
    'vk_usnap.c', 'vk_vma.cpp', 'vk_vma_budget.c', 'vk_warm_hint.h',
])
FOLDERS = dict(sorted(FOLDERS.items()))

WHERE = {name: folder for folder, names in FOLDERS.items() for name in names}
assert len(WHERE) == sum(len(v) for v in FOLDERS.values()), 'a name is in two folders'
assert not VK_ELSEWHERE & set(FOLDERS['vk'])


def git(*args, **kw):
    return subprocess.run(['git', *args], cwd=ROOT, check=True, text=True, capture_output=True, **kw).stdout


def move():
    missing = [n for n in WHERE if not (RT / n).is_file()]
    if missing:
        sys.exit(f'not at the top of runtime/: {", ".join(missing)}')
    for folder, names in FOLDERS.items():
        (RT / folder).mkdir(exist_ok=True)
        git('mv', '-k', *[f'runtime/{n}' for n in names], f'runtime/{folder}/')
        print(f'runtime/{folder}/: {len(names)} files')


NAMES = '|'.join(re.escape(n) for n in sorted(WHERE, key=len, reverse=True))
TEXT = re.compile(r'(runtime([/\\]))(' + NAMES + r')(?!\w|\.\w)')
JOIN = re.compile(r"""((?:'runtime'|"runtime"|\b(?:rt|RT|RUNTIME|runtime|rt_dir|RT_DIR))\s*/\s*)(['"])(""" + NAMES +
                  r')\2')


def rewrite_text(text, python):
    text = TEXT.sub(lambda m: f'{m.group(1)}{WHERE[m.group(3)]}{m.group(2)}{m.group(3)}', text)
    if python:
        text = JOIN.sub(lambda m: f'{m.group(1)}{m.group(2)}{WHERE[m.group(3)]}{m.group(2)} / '
                                  f'{m.group(2)}{m.group(3)}{m.group(2)}', text)
    return text


KEEP = ('tools/xlate/', 'tools/vendor/')


def rewrite(files):
    if not files:
        names = git('grep', '-lE', r'runtime[/\\]|\b(rt|RT|RUNTIME|runtime|rt_dir|RT_DIR)[\'"]?\s*/', '--', '.',
                    ':!runtime').split()
        files = [n for n in names if n != 'tools/runtime_folders.py' and not n.startswith(KEEP)]
    changed = 0
    for f in files:
        p = ROOT / f if not Path(f).is_absolute() else Path(f)
        try:
            text = p.read_text(encoding='utf-8')
        except UnicodeDecodeError:
            continue
        new = rewrite_text(text, p.suffix == '.py')
        if new != text:
            p.write_bytes(new.encode('utf-8'))
            changed += 1
            print(f'{f}: {sum(a != b for a, b in zip(text.splitlines(), new.splitlines()))} lines')
    print(f'{changed} files rewritten')


def table():
    print('| Folder | Files | Moved there |')
    print('|---|---|---|')
    for folder, names in FOLDERS.items():
        print(f'| `{folder}/` | {len(names)} | {", ".join(f"`{n}`" for n in names)} |')


def main():
    ap = argparse.ArgumentParser(description=DESCRIPTION)
    sub = ap.add_subparsers(dest='cmd', required=True)
    sub.add_parser('move')
    r = sub.add_parser('rewrite')
    r.add_argument('files', nargs='*')
    w = sub.add_parser('where')
    w.add_argument('names', nargs='+')
    sub.add_parser('table')
    a = ap.parse_args()
    if a.cmd == 'move':
        move()
    elif a.cmd == 'rewrite':
        rewrite(a.files)
    elif a.cmd == 'where':
        for n in a.names:
            print(f'{WHERE[n]}/{n}' if n in WHERE else n)
    else:
        table()


if __name__ == '__main__':
    main()
