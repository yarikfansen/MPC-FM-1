#!/bin/sh
# Lists calls AND data reads (const tables, address loads) from our audio RAM
# code (.ram0_data) into Flash (.text), using build/dx7/sdk.elf. Audio code
# must not touch Flash at all: it may run while Flash is written.
# Usage (Git Bash, repo root, after sh tools/fm1-build.sh): sh tests/check_audio_flash.sh
cd "$(dirname "$0")/.." || exit 1
OD=${OD:-/c/JL/pi32/bin/llvm-objdump.exe}
# Prefixes end in _ and match whole families (mfx_run, run_verb, fx_clear, micro_d_render).
OURS='^(fm1_audio_irq|fm1_audio_copy|fm1_output_mix|mfx_[A-Za-z0-9_.]*|run_[A-Za-z0-9_.]*|fx_[A-Za-z0-9_.]*|eq_design|chime_mix|scope_capture|fsqrt_|flog2|mix_of|micro_d_[A-Za-z0-9_.]*|render_slot|note_event|mono_play|play_event|binaural_[A-Za-z0-9_.]*|soft_limit|rec_capture|groove_midi_note|groove_midi_mode|enqueue|barrier):$'
"$OD" -d -j .ram0_data build/dx7/sdk.elf 2>/dev/null | awk -v re="$OURS" '
  /^[A-Za-z_][^ ]*:$/ { fn = $0; ours = fn ~ re }
  ours && /call/ && /<[^>]* : 20[0-9a-f]+ >/ { print fn " call " $0; bad = 1 }
  ours && !/call|goto/ && /<[^>]* : 20[0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f] >/ { print fn " data " $0; bad = 1 }
  END { exit bad }'
status=$?
[ $status -eq 0 ] && echo "audio RAM code: no calls or data reads into Flash" || echo "audio RAM code: Flash access listed above"
exit $status
