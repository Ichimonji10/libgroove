### Version 4.2.1 (2014-10-07)

 * updating metadata: set time_base on stream not the codec
 * libav lockmgr: set mutex pointer to NULL on destroy
 * fix build on GNU/hurd

### Version 4.2.0 (2014-09-25)

 * build: remove bundled dependencies
 * build: simpler cmake find modules, both code and license
 * player: add shim to fix build failure on OSX

### Version 4.1.1 (2014-06-20)

 * playlist: fix race condition which can cause decoder to hang
 * dummy player: fix timing issues

### Version 4.1.0 (2014-06-13)

 * playlist: added `groove_playlist_set_fill_mode`. Allows you to choose between
   buffering until all sinks are full and buffering until any sinks are full.

### Version 4.0.4 (2014-06-03)

 * Fixed a race condition where seeking to a different playlist item and then
   back would have a window of time where it could report the wrong position.
 * Properly play and pause network streams.

### Version 4.0.3 (2014-05-31)

 * build: update bundled libav to latest stable 10 release
 * build: link player with -lrt for clock_gettime. closes #67
 * playlist: fix case where filter graph was not being rebuilt. closes #65
 * playlist: fix race condition segfault when attaching a sink
 * encoder: properly reset encoding when flush or playlist end is encountered.
   closes #66

### Version 4.0.2 (2014-05-20)

 * player: thread cleanup only if thread was initialized - fixes potential
   crash on player detach
 * build: look for includes in the current source tree. Fixes an issue when a
   previous version of the library is installed.
 * build: on unix link with -lm

### Version 4.0.1 (2014-05-13)

 * groove_playlist_get_position: always set seconds even when item is NULL
 * playlist: correct generation of the sink map
   - fixes potential error when adding multiple sinks
   - optimizes some cases where sinks can share filter graph chain
   - dummy player now uses disable_resample optimization
 * dummy player: avoid floating point error accumulation

### Version 4.0.0 (2014-05-12)

 * GrooveBuffer struct contains the presentation time stamp
 * move include statements to outside of extern C
 * ability to set true peak on playlist items. closes #50
 * support per-sink gain adjustment. closes #41
 * GroovePlaylist: `volume` renamed to `gain`
   - `groove_playlist_set_gain` renamed to `groove_playlist_set_item_gain`
   - `groove_playlist_set_volume` renamed to `groove_playlist_set_gain`
 * player: specify device by index rather than name. closes #44
 * player: ability to attach a dummy player device. closes #60
 * fingerprinter: encode/decode return 0 on success, < 0 on error
 * fingerprinter: info struct contains raw fingerprint instead of
   compressed string. closes #61

### Version 3.1.1 (2014-04-21)

 * fingerprinter example: add --raw option
 * fingerprinter sink: add error checking to chromaprint function calls
 * fingerprinter sink: fix documentation. raw fingerprints are signed 32-bit
   integers, not unsigned 32-bit integers.
 * fingerprinter sink: change `void *` to `int32_t *` for encode/decode
   functions

### Version 3.1.0 (2014-04-19)

 * add acoustid fingerprinter sink. Closes #19
 * build: revert GNUInstallDirs
 * update to libav 10.1

### Version 3.0.8 (2014-04-01)

 * loudness scanning: fix memory corruption when scanning large album
 * update to libav10
 * playlist: fix segfault on out of memory
 * playlist: fix segfault race condition when adding a sink

### Version 3.0.7 (2014-03-16)

 * build: fix cmake warnings
 * use ebur128 true peak instead of sample peak
 * fix bug where accessing "album" metadata would instead
   return the "album_artist"
 * update to libav 10 beta2
 * use the compand filter to allow setting the gain to > 1.0. Closes #45
 * log error when cannot set up filter graph
 * loudness scanning: fix crash for songs with 0 frames. Closes #48
 * playlist example: fix race condition

### Version 3.0.6 (2014-02-20)

 * build: avoid useless dependencies

### Version 3.0.5 (2014-02-20)

 * update to libav dff1c19140e70. Closes #16 ASF seeking bug
 * build: use GNUInstallDirs

### Version 3.0.4 (2014-02-09)
 
 * delete SDL2 config-generated files from repo
   (they were causing an issue with debian packaging)

### Version 3.0.3 (2014-02-09)

 * update libav to 246d3bf0ec

### Version 3.0.2 (2013-11-25)

 * build: add static targets to all libraries

### Version 3.0.1 (2013-11-25)

 * build: depend on system libav if possible

### Version 3.0.0 (2013-11-24)

 * queue: depend on pthread instead of SDL2
 * file: depend on pthreads instead of SDL2
 * isolate SDL dependency to player
 * encoder: depend on pthread instead of SDL2
 * player: use pthread for mutexes instead of SDL2
 * loudness detector: depend on pthread instead of SDL2
 * playlist: use pthread for mutexes instead of SDL2
 * separate independent components into their own librares. closes #39
 * build: use the same version info for all libs

### Version 2.0.4 (2013-11-23)

 * update libav to d4df02131b5522
 * playlist: set sent_end_of_q at playlist create
 * better timestamp setting

### Version 2.0.3 (2013-11-22)

 * fix build when libspeexdsp is installed
 * cmake: support 2.8.7
 * groove.h conforms to C90
 * buffer implemented with pthreads instead of SDL
 * playlist: fix GrooveBuffer ref/unref race condition. closes #28

### Version 2.0.2 (2013-11-19)

 * out-of-tree build for bundled libav
 * update libav to 16e7b189c54

### Version 2.0.1 (2013-11-18)

 * compile with -O3
 * update libav to 1c01b0253eb
 * build system: bundle SDL2 but try to use system SDL2 if possible
 * when doing bundled SDL2, use the cmake build
 * enable SDL_File because osx needs it
 * try to build against system libebur128 and fall back on bundled. closes #38

### Version 2.0.0 (2013-11-16)

 * decode: remove last_decoded_file
 * SDL2 bundled dependency cleanup
 * Makefile: libgroove.so links against -ldl
 * fix 100% CPU by not disabling sdl timer. closes #20
 * playlist example: check for failure to create player
 * decode thread: end of playlist sentinel and full checking for every sink
 * update init_filter_graph to route based on sinks
 * sink: separate create from attach / destroy from detach
 * depend on system SDL2
 * expose GrooveSink buffer_size to public API
 * rename GroovePlayer to GroovePlaylist
 * rename player.c to playlist.c
 * rename GrooveDeviceSink to GroovePlayer
 * rename device_sink.c to player.c
 * rename sample_count to frame_count
 * print error string when cannot create aformat filter
 * playlist: protect sink_map with a mutex
 * player: flush queue on detach; reset queue on attach
 * rename LICENSE to COPYING as per ubuntu suggestion
 * encoder sink implementation
 * transcoding example
 * workaround for av_guess_codec bug
 * fix some memory leaks found with valgrind
 * fix segfault when logging is on for file open errors
 * fix segfault when playlist item deleted
 * logging: no audio stream found is INFO not ERROR
 * encoder: stop when encoding buffer full. closes #32
 * file: loggeng unrecognized format is INFO not ERROR
 * encoder: default bit_rate to 256kbps
 * support setting buffer_frame_count. closes #33
 * transcode example: ability to join multiple tracks into one
 * encoder: add flushes to correctly obtain format header
 * encoder: set pts and dts to stream->nb_samples. closes #34
 * playlist: decode_head waits on a mutex condition instead of sleeping
 * playlist: when sinks are full wait on mutex condition
 * encoder: draining uses a mutex condition instead of delay. closes #24
 * transcode example: copy metadata to new file. closes #31
 * encoder: fix deadlock race condition
 * add missing condition signals
 * consistent pointer conventions in groove.h
 * avoid prefixing header defines with underscores
 * better public/private pattern for GrooveFile. see #37
 * queue: better public/private pattern and no typedefs
 * encoder: better public/private pattern
 * buffer: better public/private pattern and get rid of typedef
 * player: better public/private pattern and remove typedefs
 * playlist: better public/private pattern and remove typedefs
 * remove typedef on GrooveTag. see #36
 * scan: better public/private pattern and remove typedefs
 * add license information to each file. closes #29
 * use proper prototypes when no arguments
 * do not use atexit; provide groove_finish
 * encoder: fix cleanup deadlock race condition
 * playlist: fix small memory leak on cleanup
 * player: delete unused field
 * loudness detector rewrite. closes #27 and #25
 * LoudnessDetector also reports duration. closes #23
 * loudness detector: more error checking
 * readme update
 * loudness detector: ability to get position (progress)
 * add _peek() methods to all sinks
 * remove groove_loudness_to_replaygain from API
 * loudness detector: faster and more accurate album info
 * add groove_encoder_position API
 * update libav to 72ca830f511fcdc
 * build with cmake
 * fix build errors with clang
 * cmake: only do -Wl,-Bsymbolic on unix environments
 * fix build on OSX

### Version 1.0.0 (2013-08-08)

  * initial public release
