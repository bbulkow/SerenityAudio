/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering
  Copyright 2006 Pierre Ossman <ossman@cendio.se> for Cendio AB

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2.1 of the License,
  or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with PulseAudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.

*
* Modified by bbulkow 7/7/2019 to compile and run with PulseAudio 10.0
* All modifications available under same license.
*

***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <signal.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>
#include <locale.h>
#include <stdbool.h>

#include <sndfile.h>

#include <pulse/pulseaudio.h>


// This is the structure you fill in order to play a sound.
// It will have a pointer to the file, filename, current buffer,
// maybe even eventually things like a starttime
typedef struct sa_soundplay {
	pa_stream *stream;
	char *stream_name;
	int verbose;
	pa_volume_t volume;
  SNDFILE* sndfile;
  pa_sample_spec sample_spec; // is this valid c?  
  pa_channel_map channel_map;
  bool channel_map_set;
	sf_count_t (*readf_function)(SNDFILE *_sndfile, void *ptr, sf_count_t frames);
} sa_soundplay_t;

static pa_context *g_context = NULL;
static pa_mainloop_api *g_mainloop_api = NULL;

static char *g_client_name = NULL, *g_device = NULL;
static pa_channel_map g_channel_map;
static bool g_channel_map_set = false;

static int g_verbose = 0;
static pa_volume_t g_volume = PA_VOLUME_NORM;

static sa_soundplay_t *g_player = NULL;

static pa_time_event *g_timer = NULL;

// My timer will fire every 50ms
//#define TIME_EVENT_USEC 50000
#define TIME_EVENT_USEC 1000000

/* a forward reference */
static void sa_soundplay_free(sa_soundplay_t *);

/* A shortcut for terminating the application */
static void quit(int ret) {
    assert(g_mainloop_api);
    g_mainloop_api->quit(g_mainloop_api, ret);
}

/* Connection draining complete */
static void context_drain_complete(pa_context *c, void *userdata) {
    pa_context_disconnect(c);
}

/* Stream draining complete */
static void stream_drain_complete(pa_stream *s, int success, void *userdata) {
    sa_soundplay_t *splay = (sa_soundplay_t *) userdata;
    pa_operation *o;

    if (!success) {
        fprintf(stderr, "Failed to drain stream: %s\n", pa_strerror(pa_context_errno(g_context)));
        quit(1);
    }

    if (splay->verbose)
        fprintf(stderr, "Playback stream drained.\n");

    pa_stream_disconnect(splay->stream);
    pa_stream_unref(splay->stream);
    splay->stream = NULL;

		// draining a context seems extreme? 
    if (!(o = pa_context_drain(g_context, context_drain_complete, NULL)))
        pa_context_disconnect(g_context);
    else {
        pa_operation_unref(o);

        if (splay->verbose)
            fprintf(stderr, "Draining connection to server.\n");
    }
}

/* This is called whenever new data may be written to the stream */
static void stream_write_callback(pa_stream *s, size_t length, void *userdata) {
    
		sa_soundplay_t *splay = (sa_soundplay_t *)userdata;

    sf_count_t bytes;
    void *data;

		if (splay->verbose) fprintf(stderr,"stream write callback\n");

    assert(s && length);

    if (!splay->sndfile) {
				if (splay->verbose) fprintf(stderr, "write callback with no sndfile\n");
        return;
		}

    data = pa_xmalloc(length);

    if (splay->readf_function) {
        size_t k = pa_frame_size(&splay->sample_spec);

        if ((bytes = (splay->readf_function) (splay->sndfile, data, (sf_count_t) (length/k))) > 0)
            bytes *= (sf_count_t) k;

    } else {
        bytes = sf_read_raw(splay->sndfile, data, (sf_count_t) length);
		}

    if (bytes > 0)
        pa_stream_write(s, data, (size_t) bytes, pa_xfree, 0, PA_SEEK_RELATIVE);
    else
        pa_xfree(data);

    if (bytes < (sf_count_t) length) {
        sf_close(splay->sndfile);
        splay->sndfile = NULL;
        pa_operation_unref(pa_stream_drain(s, stream_drain_complete, userdata));
    }
}

/* This routine is called whenever the stream state changes */
static void stream_state_callback(pa_stream *s, void *userdata) {
		sa_soundplay_t *splay = (sa_soundplay_t *)userdata;

		if (splay->verbose) {
			fprintf(stderr, "stream state callback\n");
		}

		// just making sure
    assert(s);

    switch (pa_stream_get_state(s)) {
        case PA_STREAM_CREATING:
        case PA_STREAM_TERMINATED:
            break;

        case PA_STREAM_READY:
            if (splay->verbose)
                fprintf(stderr, "Stream successfully created\n");
            break;

        case PA_STREAM_FAILED:
        default:
            fprintf(stderr, "Stream errror: %s\n", pa_strerror(pa_context_errno(pa_stream_get_context(s))));
            quit(1);
    }
}

/* This is called whenever the context status changes */
/* todo: creating the stream as soon as the context comes available is kinda fun, but 
** we really want something else
*/
static void context_state_callback(pa_context *c, void *userdata) {
		sa_soundplay_t *splay = (sa_soundplay_t *)userdata;

		if (splay->verbose) {
			fprintf(stderr, "context state callback\n");
		}

		// just making sure???
    assert(c);

    switch (pa_context_get_state(c)) {
        case PA_CONTEXT_CONNECTING:
        case PA_CONTEXT_AUTHORIZING:
        case PA_CONTEXT_SETTING_NAME:
            break;

        case PA_CONTEXT_READY: {
            pa_cvolume cv;

            assert(c && !splay->stream);

            if (splay->verbose)
                fprintf(stderr, "Connection established.\n");

            splay->stream = pa_stream_new(c, splay->stream_name, &splay->sample_spec, splay->channel_map_set ? &splay->channel_map : NULL);
            assert(splay->stream);

            pa_stream_set_state_callback(splay->stream, stream_state_callback, splay);
            pa_stream_set_write_callback(splay->stream, stream_write_callback, splay);
            pa_stream_connect_playback(splay->stream, g_device, NULL/*buffer_attr*/ , 0/*flags*/ , 
								pa_cvolume_set(&cv, splay->sample_spec.channels, splay->volume), 
								NULL/*sync stream*/);

            break;
        }

        case PA_CONTEXT_TERMINATED:
            quit(0);
            break;

        case PA_CONTEXT_FAILED:
        default:
            fprintf(stderr, "Connection failure: %s\n", pa_strerror(pa_context_errno(c)));
            quit(1);
    }
}

/* UNIX signal to quit recieved */
static void exit_signal_callback(pa_mainloop_api*m, pa_signal_event *e, int sig, void *userdata) {	
    if (g_verbose)
        fprintf(stderr, "Got SIGINT, exiting.\n");
    quit(0);
}


// open it and set it for async playing
// eventually can add delays and whatnot

// Filename of null means use stdin... or is always passed in?
// filename is a static and not to be freed

static sa_soundplay_t * sa_play_file( char *filename ) {

	  sa_soundplay_t *splay = malloc(sizeof(sa_soundplay_t));
		memset(splay, 0, sizeof(sa_soundplay_t) );  // typically don't do this, do every field, but doing it this time

    SF_INFO sfinfo;

  // initialize many things from the globals at this point
    splay->channel_map_set = g_channel_map_set;
		if (splay->channel_map_set) {
			splay->channel_map = g_channel_map;
		}
		splay->volume = g_volume;
		splay->verbose = g_verbose;

	// open file
    memset(&sfinfo, 0, sizeof(sfinfo));

    if (filename != NULL)
        splay->sndfile = sf_open(filename, SFM_READ, &sfinfo);
    else
        splay->sndfile = sf_open_fd(STDIN_FILENO, SFM_READ, &sfinfo, 0);

		// Todo: have an error code
    if (!splay->sndfile) {
        fprintf(stderr, "Failed to open file '%s'\n", filename);
				sa_soundplay_free(splay);
        return(NULL);
    }

    splay->sample_spec.rate = (uint32_t) sfinfo.samplerate;
    splay->sample_spec.channels = (uint8_t) sfinfo.channels;

    splay->readf_function = NULL;

    switch (sfinfo.format & 0xFF) {
        case SF_FORMAT_PCM_16:
        case SF_FORMAT_PCM_U8:
        case SF_FORMAT_PCM_S8:
            splay->sample_spec.format = PA_SAMPLE_S16NE;
            splay->readf_function = (sf_count_t (*)(SNDFILE *_sndfile, void *ptr, sf_count_t frames)) sf_readf_short;
            break;

        case SF_FORMAT_ULAW:
            splay->sample_spec.format = PA_SAMPLE_ULAW;
            break;

        case SF_FORMAT_ALAW:
            splay->sample_spec.format = PA_SAMPLE_ALAW;
            break;

        case SF_FORMAT_FLOAT:
        case SF_FORMAT_DOUBLE:
        default:
            splay->sample_spec.format = PA_SAMPLE_FLOAT32NE;
            splay->readf_function = (sf_count_t (*)(SNDFILE *_sndfile, void *ptr, sf_count_t frames)) sf_readf_float;
            break;
    }

    if (!splay->stream_name) {
        const char *n, *sn;

				// WARNING. No information about whether this function is returning
			  // newly allocated memory that must be freed, or a pointer inside
		    // the soundfile. Could thus be a memory leak
        n = sf_get_string(splay->sndfile, SF_STR_TITLE);

        if (!n)
            n = filename;

				// this returns a string that must be freed with pa_xfree()
        splay->stream_name = pa_locale_to_utf8(n);
        if (!sn)
            splay->stream_name = pa_utf8_filter(n);

    }

    if (splay->verbose) {
        char t[PA_SAMPLE_SPEC_SNPRINT_MAX];
        pa_sample_spec_snprint(t, sizeof(t), &splay->sample_spec);
        fprintf(stderr, "Using sample spec '%s'\n", t);
		}

		return(splay);

}

static void sa_soundplay_free( sa_soundplay_t *splay ) {
  if (splay->stream) pa_stream_unref(splay->stream);
	if (splay->stream_name) pa_xfree(splay->stream_name);
  if (splay->sndfile) sf_close(splay->sndfile);

  free(splay);
}

/*
** timer - this is called frequently, and where we decide to start and stop effects.
** or loop them because they were completed or whatnot.
*/

/* pa_time_event_cb_t */
static void
sa_timer(pa_mainloop_api *a, pa_time_event *e, const struct timeval *tv, void *userdata)
{
	fprintf(stderr, "time event called: sec %d usec %d\n",tv->tv_sec, tv->tv_usec);

	struct timeval now;
	gettimeofday(&now, NULL);
	pa_timeval_add(&now, TIME_EVENT_USEC);
	a->time_restart(e,&now);
} 

static void help(const char *argv0) {

    printf("%s [options] [FILE]\n\n"
           "  -h, --help                            Show this help\n"
           "      --version                         Show version\n\n"
           "  -v, --verbose                         Enable verbose operation\n\n"
           "  -s, --server                          The name of the server to connect to\n"
           "  -d, --device=DEVICE                   The name of the sink to connect to\n"
           "  -n, --client-name=NAME                How to call this client on the server\n"
           "      --stream-name=NAME                How to call this stream on the server\n"
           "      --volume=VOLUME                   Specify the initial (linear) volume in range 0...65536\n"
             "      --channel-map=CHANNELMAP          Set the channel map to the use\n",
           argv0);
}

enum {
    ARG_VERSION = 256,
    ARG_STREAM_NAME,
    ARG_VOLUME,
    ARG_CHANNELMAP
};

int main(int argc, char *argv[]) {
    pa_mainloop* m = NULL;
    int ret = 1, r, c;
    char *bn = NULL;
		char *server = NULL;
		char *stream_name = NULL;
    const char *filename;
    SF_INFO sfinfo;

    static const struct option long_options[] = {
        {"device",      1, NULL, 'd'},
				{"server",			1, NULL, 's'},
        {"client-name", 1, NULL, 'n'},
        {"stream-name", 1, NULL, ARG_STREAM_NAME},
        {"version",     0, NULL, ARG_VERSION},
        {"help",        0, NULL, 'h'},
        {"verbose",     0, NULL, 'v'},
        {"volume",      1, NULL, ARG_VOLUME},
        {"channel-map", 1, NULL, ARG_CHANNELMAP},
        {NULL,          0, NULL, 0}
    };

    if (!(bn = strrchr(argv[0], '/')))
        bn = argv[0];
    else
        bn++;

    while ((c = getopt_long(argc, argv, "d:s:n:h", long_options, NULL)) != -1) {

        switch (c) {
            case 'h' :
                help(bn);
                ret = 0;
                goto quit;

            case 'd':
                pa_xfree(g_device);
                g_device = pa_xstrdup(optarg);
                break;

						case 's':
							  pa_xfree(server);
								server = pa_xstrdup(optarg);
								break;

            case 'n':
                pa_xfree(g_client_name);
                g_client_name = pa_xstrdup(optarg);
                break;

            case ARG_STREAM_NAME:
                pa_xfree(stream_name);
                stream_name = pa_xstrdup(optarg);
                break;

            case 'v':
                g_verbose = 1;
                break;

            case ARG_VOLUME: {
                int v = atoi(optarg);
                g_volume = v < 0 ? 0U : (pa_volume_t) v;
                break;
            }

            case ARG_CHANNELMAP:
                if (!pa_channel_map_parse(&g_channel_map, optarg)) {
                    fprintf(stderr, "Invalid channel map\n");
                    goto quit;
                }

                g_channel_map_set = true;
                break;

            default:
                goto quit;
        }
    }

    filename = optind < argc ? argv[optind] : "STDIN";

    if (!g_client_name) {
				// must be freed with pa_xfree
        g_client_name = pa_locale_to_utf8(bn);
        if (!g_client_name)
            g_client_name = pa_utf8_filter(bn);
    }

		if (g_verbose) {
			fprintf(stderr, "ready to play file %s\n",filename);	
		}

		// Create a player for the file in question
		sa_soundplay_t *splay = sa_play_file( optind < argc ? argv[optind] : "STDIN" );
		if (splay == NULL) {
			fprintf(stderr, "play file failed\n");
       goto quit;
    }

		if (g_verbose) {
			fprintf(stderr, "about to set up mainloop\n");	
		}

    /* Set up a new main loop */
    if (!(m = pa_mainloop_new())) {
        fprintf(stderr, "pa_mainloop_new() failed.\n");
        goto quit;
    }

    g_mainloop_api = pa_mainloop_get_api(m);

    r = pa_signal_init(g_mainloop_api);
    assert(r == 0);
    pa_signal_new(SIGINT, exit_signal_callback, NULL);
#ifdef SIGPIPE
    signal(SIGPIPE, SIG_IGN);
#endif

		if (g_verbose) {
			fprintf(stderr, "about to create new context \n");	
		}

    /* Create a new connection context */
		/* note: documentation says post 0.9, use with_proplist() and specify some defaults */
    if (!(g_context = pa_context_new(g_mainloop_api, g_client_name))) {
        fprintf(stderr, "pa_context_new() failed.\n");
        goto quit;
    }

    pa_context_set_state_callback(g_context, context_state_callback, splay);

    /* Connect the context */
    if (pa_context_connect(g_context, server, 0, NULL) < 0) {
        fprintf(stderr, "pa_context_connect() failed: %s", pa_strerror(pa_context_errno(g_context)));
        goto quit;
    }

		if (g_verbose) {
			fprintf(stderr, "about to run mainloop\n");	
		}

		/* set up our timer */
		struct timeval now;
		gettimeofday(&now, NULL);
		pa_timeval_add(&now, TIME_EVENT_USEC);	
		g_timer = (* g_mainloop_api->time_new) (g_mainloop_api, &now, sa_timer, NULL);
		if (g_timer == NULL) {
			fprintf(stderr, "time_new failed!!!\n");
		}

    /* Run the main loop - hangs here forever? */
    if (pa_mainloop_run(m, &ret) < 0) {
        fprintf(stderr, "pa_mainloop_run() failed.\n");
        goto quit;
    }

quit:
		if (g_verbose) {
			fprintf(stderr, "quitting and cleaning up\n");	
		}

    if (g_context)
        pa_context_unref(g_context);

    if (m) {
        pa_signal_done();
        pa_mainloop_free(m);
    }
		sa_soundplay_free(splay);

    pa_xfree(server);
    pa_xfree(g_device);
    pa_xfree(g_client_name);
    pa_xfree(stream_name);

    return ret;
}
