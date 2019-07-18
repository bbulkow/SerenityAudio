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
	pa_stream *stream; // gets reset to NULL when file is over
	char *stream_name;
	char *filename;

	int verbose;

	pa_volume_t volume;

  	SNDFILE* sndfile;
  	pa_sample_spec sample_spec; // is this valid c?  
  	pa_channel_map channel_map;
  	bool channel_map_set;

	sf_count_t (*readf_function)(SNDFILE *_sndfile, void *ptr, sf_count_t frames);
} sa_soundplay_t;

// for now, the single oneg
static char *g_filename1 = "sounds/crickets-dawn.wav";  // this is not duped, it's from the inputs
static char *g_filename2 = "sounds/bullfrog-2.wav";  // this is not duped, it's from the inputs
static sa_soundplay_t *g_splay1 = NULL;
static sa_soundplay_t *g_splay2 = NULL;


static pa_context *g_context = NULL;
static bool g_context_connected = false;

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
#define TIME_EVENT_USEC 100000

/* a forward reference */
static void sa_soundplay_start(sa_soundplay_t *);
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
        fprintf(stderr, "Playback stream %s drained.\n",splay->stream_name );

    pa_stream_disconnect(splay->stream);
    pa_stream_unref(splay->stream);
    splay->stream = NULL;

}

/* This is called whenever new data may be written to the stream */
static void stream_write_callback(pa_stream *s, size_t length, void *userdata) {
    
	sa_soundplay_t *splay = (sa_soundplay_t *)userdata;

    sf_count_t bytes;
    void *data;

	//if (splay->verbose) fprintf(stderr,"stream write callback\n");

    assert(s && length);

    if (!splay->sndfile) {
		if (splay->verbose) fprintf(stderr, "write callback with no sndfile %s\n",splay->stream_name);
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
		fprintf(stderr, "stream state callback: %d\n",pa_stream_get_state(s) );
	}

	// just making sure
    assert(s);

    switch (pa_stream_get_state(s)) {
        case PA_STREAM_CREATING:
        	break;
        case PA_STREAM_TERMINATED:
        	if (splay->verbose) fprintf(stderr, "stream %s terminated\n",splay->stream_name);
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

	if (g_verbose) {
		fprintf(stderr, "context state callback, new state %d\n",pa_context_get_state(c) );
	}

		// just making sure???
    assert(c);

    switch (pa_context_get_state(c)) {
        case PA_CONTEXT_CONNECTING:
        case PA_CONTEXT_AUTHORIZING:
        case PA_CONTEXT_SETTING_NAME:
            break;

        case PA_CONTEXT_READY: {
            assert(c);

            if (g_verbose)
                fprintf(stderr, "Connection established.\n");
            g_context_connected = true;

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

static sa_soundplay_t * sa_soundplay_new( char *filename ) {

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

	splay->sndfile = sf_open(filename, SFM_READ, &sfinfo);
    splay->filename = filename;

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

    // better have had a context - don't know if it's connected though?
    assert(g_context);

    if (splay->verbose) {
        char t[PA_SAMPLE_SPEC_SNPRINT_MAX];
        pa_sample_spec_snprint(t, sizeof(t), &splay->sample_spec);
        fprintf(stderr, "created play file using sample spec '%s'\n", t);
		}

	return(splay);

}

static void sa_soundplay_start( sa_soundplay_t *splay) {

	if (splay->stream) {
		fprintf(stderr, "Called start on already playing stream %s\n",splay->stream_name);
		return;
	}
	if (splay->verbose) fprintf(stderr, "soundplay start: %s\n",splay->stream_name);

	// have to open a new soundfile, but already have the key parameters
	if (splay->sndfile==NULL) {

    	SF_INFO sfinfo;
	    memset(&sfinfo, 0, sizeof(sfinfo));

        splay->sndfile = sf_open(splay->filename, SFM_READ, &sfinfo);
        assert(splay->sndfile);
    }

    splay->stream = pa_stream_new(g_context, splay->stream_name, &splay->sample_spec, splay->channel_map_set ? &splay->channel_map : NULL);
    assert(splay->stream);

	pa_cvolume cv;

    pa_stream_set_state_callback(splay->stream, stream_state_callback, splay);
    pa_stream_set_write_callback(splay->stream, stream_write_callback, splay);
    pa_stream_connect_playback(splay->stream, g_device, NULL/*buffer_attr*/ , 0/*flags*/ , 
				pa_cvolume_set(&cv, splay->sample_spec.channels, splay->volume), 
			NULL/*sync stream*/);

}

// terminate a given sound
static void sa_soundplay_terminate( sa_soundplay_t *splay) {
	if (splay->stream) {
		pa_stream_disconnect(splay->stream);
		if (splay->verbose) {
			fprintf(stderr, "terminating stream %s will get a callback for draining",splay->stream_name);
		}
	}
	else {
		fprintf(stderr, "soundplay_terminate but no stream in progress");
	}

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

static struct timeval g_start_time;
static bool g_started = false;

/* pa_time_event_cb_t */
static void
sa_timer(pa_mainloop_api *a, pa_time_event *e, const struct timeval *tv, void *userdata)
{
	if (g_verbose) fprintf(stderr, "time event called: sec %d usec %d\n",tv->tv_sec, tv->tv_usec);

	// FIRST TIME AFTER CONTEXT IS CONNECTED
	if ( (g_started == false) && (g_context_connected == true)) {

		if (g_verbose) fprintf(stderr, "first time started\n");

		// Create a player for each file
		sa_soundplay_t *splay;
		splay = sa_soundplay_new( g_filename1 );
		if (splay == NULL) {
			fprintf(stderr, "play file1 failed\n");
	       goto ABORT;
		}
		sa_soundplay_start(splay);
		g_splay1 = splay; // for freeing only

		splay = sa_soundplay_new( g_filename2 );
		if (splay == NULL) {
			fprintf(stderr, "play file1 failed\n");
	       goto ABORT;
		}
		sa_soundplay_start(splay);
		g_splay2 = splay; // for freeing only


		g_started = true;
	}
	else {
		if (g_splay1->stream == NULL) {
			sa_soundplay_start(g_splay1);
		}

		if (g_splay2->stream == NULL) {
			sa_soundplay_start(g_splay2);
		}

	}

	// put the things you want to happen in here
ABORT:	;

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

    if (!g_client_name) {
				// must be freed with pa_xfree
        g_client_name = pa_locale_to_utf8(bn);
        if (!g_client_name)
            g_client_name = pa_utf8_filter(bn);
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
    g_context = pa_context_new(g_mainloop_api, g_client_name);
    if (!g_context) {
        fprintf(stderr, "pa_context_new() failed.\n");
        goto quit;
    }

    pa_context_set_state_callback(g_context, context_state_callback, NULL);

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
	sa_soundplay_free(g_splay1);
	g_splay1 = NULL;
	sa_soundplay_free(g_splay2);
	g_splay2 = NULL;

    pa_xfree(server);
    pa_xfree(g_device);
    pa_xfree(g_client_name);
    pa_xfree(stream_name);

    return ret;
}
