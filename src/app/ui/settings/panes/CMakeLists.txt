# usbbridge.pane.c is ours. v1.2.3 removed audio.pane.c and video.pane.c from
# this list, and ours sits on the same two lines -- so this conflicts on every
# release that touches it. Take his list, add ours back.
target_sources(moonlight-lib PRIVATE basic.pane.c host.pane.c input.pane.c
        experimental.pane.c usbbridge.pane.c av_pane.c pref_obj.c pref_fps.c pref_res.c)
