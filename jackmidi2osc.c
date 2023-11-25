/* JACK MIDI To OSC
 *
 * (C) 2013,2015 Robin Gareus <robin@gareus.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 */

#ifdef WIN32
#include <windows.h>
#include <pthread.h>
#define pthread_t //< override jack.h def
#endif

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <getopt.h>
#include <math.h>
#include <sys/stat.h>
#include <assert.h>

#ifndef WIN32
#include <sys/mman.h>
#include <signal.h>
#include <pthread.h>
#endif

#include <jack/jack.h>
#include <jack/transport.h>
#include <jack/ringbuffer.h>
#include <jack/midiport.h>

#include <lo/lo.h>

#ifndef RINGBUF_SIZE
#define RINGBUF_SIZE 64
#endif

#ifndef PATH_MAX
#define PATH_MAX 1024
#endif

#ifndef MAX_CFG_LINE_LEN
#define MAX_CFG_LINE_LEN 1024
#endif

#ifndef PRIu32
#define PRIu32 "u"
#endif


/* jack connection */
jack_client_t *j_client = NULL;
jack_port_t   *j_input_port;

/* threaded communication */
static jack_ringbuffer_t *rb = NULL;
static pthread_mutex_t msg_thread_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t data_ready = PTHREAD_COND_INITIALIZER;

/* application state */

static volatile enum {Terminate, Starting, Running} run = Starting;
static int dropped_messages = 0;
static double samplerate = 48000.0;

/* parameters & options */
static char *j_connect     = NULL;
static char *cfgfile       = NULL; // use default /etc/... ?
static lo_address osc_dest = NULL;
static int want_verbose    = 0;
static enum {SyncImmediate, SyncRelative, SyncAbsolute} sync_mode = SyncImmediate;

/* MIDI to OSC map / rules */
typedef struct {
	char      path[1024];
	char      desc[16];
	char    **param;
} OSCMessageTemplate;

typedef struct {
	uint8_t             mask[3];
	uint8_t             match[3];
	uint8_t             len;
	unsigned int        message_count;
	OSCMessageTemplate *msg;
} Rule;

Rule *rules = NULL;
unsigned int rule_count = 0;

/* message passing */
typedef struct {
	jack_nframes_t tme;
	uint8_t        d[3];
	uint8_t        len;
} MidiMessage;

static int process_jmidi_event (jack_midi_event_t *ev, const jack_nframes_t tme) {
	if (ev->size < 1 || ev->size > 3) {
		return 0;
	}

	if (jack_ringbuffer_write_space (rb) >= sizeof (MidiMessage)) {
		MidiMessage mmsg;

		mmsg.tme = tme + ev->time;
		mmsg.d[0] = ev->buffer[0];

		if (ev->size == 1) {
			mmsg.len = 1;
			mmsg.d[1] = 0;
			mmsg.d[2] = 0;
		} else if (ev->size == 2) {
			mmsg.len = 2;
			mmsg.d[1] = ev->buffer[1];
			mmsg.d[2] = 0;
		} else {
			mmsg.len = 3;
			mmsg.d[1] = ev->buffer[1];
			mmsg.d[2] = ev->buffer[2];
		}

		jack_ringbuffer_write (rb, (void *) &mmsg, sizeof (MidiMessage));
		return 1;
	}

	++dropped_messages;
	return 0;
}

/* jack process callback */
static int process (jack_nframes_t nframes, void *arg) {
	if (run != Running) return 0;

	const uint64_t frametime = jack_last_frame_time(j_client) + ((sync_mode == SyncRelative) ? nframes : 0);

	int n;
	int wakeup = 0;
	void *in_buf = jack_port_get_buffer (j_input_port, nframes);
	int nevents = jack_midi_get_event_count (in_buf);

	for (n = 0; n < nevents; ++n) {
		jack_midi_event_t ev;
		jack_midi_event_get (&ev, in_buf, n);
		wakeup |= process_jmidi_event (&ev, frametime);
	}

	// notify main thread
	if (wakeup) {
		if (pthread_mutex_trylock (&msg_thread_lock) == 0) {
			pthread_cond_signal (&data_ready);
			pthread_mutex_unlock (&msg_thread_lock);
		}
	}

	return 0;
}

/* callback if jack server terminates */
static void jack_shutdown (void *arg) {
	j_client=NULL;
	pthread_cond_signal (&data_ready);
	fprintf (stderr, "jack server shutdown\n");
}

/* cleanup and exit */
static void cleanup (void) {
	int i;
	if (j_client) {
		jack_deactivate (j_client);
		jack_client_close (j_client);
	}
	if (rb) {
		jack_ringbuffer_free (rb);
	}

	for (i = 0; i < rule_count; ++i) {
		int j;
		const unsigned int mc = rules[i].message_count;
		for (j = 0; j < mc; ++j) {
			int k;
			const unsigned int pl = strlen (rules[i].msg[j].desc);
			for (k = 0; k < pl; ++k) {
				free(rules[i].msg[j].param[k]);
			}
			free (rules[i].msg[j].param);
		}
		free (rules[i].msg);
	}

	if (osc_dest) {
		lo_address_free (osc_dest);
	}

	free(rules);
	free(cfgfile);
	free (j_connect);

	rules = NULL;
	cfgfile = NULL;
	j_client = NULL;
	j_connect = NULL;
	osc_dest = NULL;
}

/* open a client connection to the JACK server */
static int init_jack (const char *client_name) {
	jack_status_t status;
	j_client = jack_client_open (client_name, JackNullOption, &status);
	if (j_client == NULL) {
		fprintf (stderr, "jack_client_open () failed, status = 0x%2.0x\n", status);
		if (status & JackServerFailed) {
			fprintf (stderr, "Unable to connect to JACK server\n");
		}
		return (-1);
	}
	if (status & JackServerStarted) {
		fprintf (stderr, "JACK server started\n");
	}
	if (status & JackNameNotUnique) {
		client_name = jack_get_client_name (j_client);
		fprintf (stderr, "jack-client name: `%s'\n", client_name);
	}
	jack_set_process_callback (j_client, process, 0);
	samplerate = (double) jack_get_sample_rate (j_client);

	jack_on_shutdown (j_client, jack_shutdown, NULL);
	return (0);
}

static int jack_portsetup (void) {
	if ((j_input_port = jack_port_register (j_client, "in", JACK_DEFAULT_MIDI_TYPE, JackPortIsInput, 0)) == 0) {
		fprintf (stderr, "cannot register MIDI input port !\n");
		return (-1);
	}
	return (0);
}

static int inport_connect (char *port) {
	if (port && strlen(port) < 1) {
		return 0;
	}
	if (port && jack_connect (j_client, port, jack_port_name (j_input_port))) {
		fprintf (stderr, "cannot connect port %s to %s\n", port, jack_port_name (j_input_port));
		return 1;
	}
	return 0;
}


/******************************************************************************
 * Configuration & Rules
 */

#ifdef _WIN32
static char * strndup (const char *s, size_t n) {
	size_t len = strlen (s);

	if (n < len) {
		len = n;
	}

	char *rv = (char *) malloc (len + 1);
	if (!rv) {
		return 0;
	}

	rv[len] = '\0';
	return memcpy (rv, s, len);
}
#endif

static int append_osc_message (Rule *r, const char *path, const char *desc, const char *param) {
	assert (path);
	assert (desc);
	assert (param);

	const unsigned int mi = r->message_count;
	r->msg = realloc(r->msg, (++r->message_count) * sizeof(OSCMessageTemplate));
	if (!r->msg) {
		r->message_count = 0;
		fprintf (stderr, "Failed to allocate memory for message\n");
		return -1;
	}

	strncpy(r->msg[mi].path, path,   sizeof(r->msg[mi].path));
	strncpy(r->msg[mi].desc, desc,   sizeof(r->msg[mi].desc));
	r->msg[mi].param = NULL;

	const unsigned int pl = strlen(desc);
	if (pl == 0) {
		return 0;
	}

	const char *t0 = param;
	r->msg[mi].param = (char**) calloc (pl, sizeof(char*));

	unsigned int j;
	for (j = 0; j < pl; ++j) {
		assert (t0);

		while (*t0 && *t0 != '"') { ++t0; }
		if (!*t0) { break; }
		assert (*t0 == '"');
		++t0;
		while (desc[j] != 's' && *t0 && *t0 == ' ') { ++t0; }
		if (!*t0) { break; }

		const char *tmp = strchr(t0, '"'); // TODO ignore escaped quotes

		if (!tmp) { break; }

		if (tmp == t0) {
			if (desc[j] != 's') break;
			r->msg[mi].param[j] = strdup("");
		} else {
			r->msg[mi].param[j] = strndup(t0, tmp - t0);
		}
		t0 = ++tmp;
	}

	if (j != pl) {
		fprintf (stderr, "Invalid Config, expected %d parameters, got %d.\n", pl, j + 1);
		for (j = 0; j < pl; ++j) {
			free(r->msg[mi].param[j]);
		}
		free(r->msg[mi].param);
		--r->message_count;
		return -1;
	}
	return 0;
}

static Rule *new_rule (const char *flt) {
	const unsigned int rc = rule_count;
	rules = (Rule*) realloc(rules, (++rule_count) * sizeof(Rule));
	if (!rules) {
		fprintf (stderr, "Out of memory for rule(s).\n");
		rule_count = 0;
		return NULL;
	}

	Rule *r = &rules[rc];
	memset(r, 0, sizeof(Rule));

	char *tmp, *fre, *prt;
	int param[2];
	int i = 0;

	tmp = fre = strdup(flt);
	for (prt = strtok(tmp, " "); prt; prt = strtok(NULL, " "), ++i) {
		if (i >= 3) {
			i = -1;
			break;
		}
		if (!strcasecmp(prt, "ANY")) {
			r->mask[i] = 0x00; r->match[i] = 0x00;
		} else if (i == 0 && !strcasecmp(prt, "NOTE")) {
			r->mask[i] = 0xe0; r->match[i] = 0x80;
		} else if (i == 0 && !strcasecmp(prt, "NOTEOFF")) {
			r->mask[i] = 0xf0; r->match[i] = 0x80;
		} else if (i == 0 && !strcasecmp(prt, "NOTEON")) {
			r->mask[i] = 0xf0; r->match[i] = 0x90;
		} else if (i == 0 && !strcasecmp(prt, "KeyPressure")) {
			r->mask[i] = 0xf0; r->match[i] = 0xa0; // Aftertouch, 3 bytes
		} else if (i == 0 && !strcasecmp(prt, "CC")) {
			r->mask[i] = 0xf0; r->match[i] = 0xb0;
		} else if (i == 0 && !strcasecmp(prt, "PGM")) {
			r->mask[i] = 0xf0; r->match[i] = 0xc0;
		} else if (i == 0 && !strcasecmp(prt, "ChanPressure")) {
			r->mask[i] = 0xf0; r->match[i] = 0xd0; // Aftertouch, 2 bytes
		} else if (i == 0 && !strcasecmp(prt, "Pitch")) {
			r->mask[i] = 0xf0; r->match[i] = 0xe0;
		} else if (i == 0 && !strcasecmp(prt, "Pos")) {
			r->mask[i] = 0xff; r->match[i] = 0xf2; // Song Position Pointer, 3 bytes
		} else if (i == 0 && !strcasecmp(prt, "Song")) {
			r->mask[i] = 0xff; r->match[i] = 0xf3; // Song select 2 bytes
		} else if (i == 0 && !strcasecmp(prt, "Start")) {
			r->mask[i] = 0xff; r->match[i] = 0xfa; // rt 1 byte
		} else if (i == 0 && !strcasecmp(prt, "Cont")) {
			r->mask[i] = 0xff; r->match[i] = 0xfb; // rt 1 byte
		} else if (i == 0 && !strcasecmp(prt, "Stop")) {
			r->mask[i] = 0xff; r->match[i] = 0xfc; // rt 1 byte
		} else if (2 == sscanf (prt, "%i/%i", &param[0], &param[1])) {
			r->mask[i] = param[1] & 0xff;
			r->match[i] = param[0] & 0xff;
		} else if (1 == sscanf (prt, "%i", &param[0])) {
			r->mask[i] = (i == 0) ? 0xff : 0x7f;
			r->match[i] = param[0] & 0xff;
		} else {
			fprintf(stderr, "Failed to parse rule filter\n");
			i = -1;
			break;
		}
	}
	free (fre);
	if (i < 1 || i > 3) {
		fprintf(stderr, "Invalid filter rule...\n");
		--rule_count;
		return NULL;
	}
	// TODO sanity check  message-type, len
	r->len = i; // TODO allow 'len=0' catch all
	return r;
}

static int parse_osc_addr (const char *arg) {
	char addr[1024];
	char port[64];
	if (2 == sscanf (arg, "%[^:]:%[^:]", addr, port)) {
		if (osc_dest) { lo_address_free (osc_dest); }
		osc_dest = lo_address_new (addr, port);
	} else if (atoi (arg) > 0 && atoi (arg) < 65536) {
		if (osc_dest) { lo_address_free (osc_dest); }
		snprintf (port, sizeof (port), "%d", atoi (arg));
		osc_dest = lo_address_new (NULL, port);
	} else {
		fprintf (stderr, "given OSC address '%s' is not valid\n\n", arg);
		return -1;
	}
	return 0;
}

static int parse_sync_mode (const char *arg) {
	if (!arg || strlen(arg) < 1) { return -1; }
	size_t cl = strlen(arg);
	if      (!strncasecmp(arg, "Immediate", cl)) { sync_mode = SyncImmediate; }
	else if (!strncasecmp(arg, "Absolute", cl))  { sync_mode = SyncAbsolute; }
	else if (!strncasecmp(arg, "Relative", cl))  { sync_mode = SyncRelative; }
	else { return -1; }
	return 0;
}

static int read_config (const char *configfile) {
	FILE *f;
	char line[MAX_CFG_LINE_LEN];

	if (!(f = fopen(configfile, "r"))) {
		fprintf (stderr, "Cannot open config '%s' for reading.\n", configfile);
		return -1;
	} else {
		printf ("Reading config '%s'\n", configfile);
	}

	int rv = 0;
	int lineno = 0;
	Rule *r = NULL;
	enum {NoRule, StartRule, InRule, InConfig} parser_state = NoRule;

	// read file line by line
	while (fgets (line, MAX_CFG_LINE_LEN - 1, f) != NULL ) {
		++lineno;

		if (strlen(line) == MAX_CFG_LINE_LEN - 1) {
			fprintf (stderr, "Too long line: %d\n", lineno);
			continue;
		}
		
		while (strlen(line) > 0 && (line[strlen(line) - 1] == '\n' || line[strlen(line) - 1] == '\r' || line[strlen(line) - 1] == ' ' || line[strlen(line) - 1] == '\t')) {
			line[strlen(line) - 1] = '\0';
		}

		if (strlen(line) == 0 || line[0] == '#') {
			continue;
		}

		if (!strcmp (line, "[config]")) {
			if (parser_state == StartRule) {
				rv = -1;
				goto parser_end;
			}
			parser_state = InConfig;
		}
		else if (!strcmp (line, "[rule]")) {
			if (parser_state == StartRule) {
				rv = -1;
				goto parser_end;
			}
			parser_state = StartRule;
		}
		else if (parser_state == InRule) {
			assert (r);
			// TODO split properly, check lengths, allow escaped quotes in path
			char a[1024], b[16], c[1024];
			if (3 == sscanf (line, "\"%[^\"]\" \"%[^\"]\" %1023c", a, b, c)) {
				if (append_osc_message(r, a, b, c)) {
					fprintf (stderr, "Failed to append/parse OSC message from line: %d\n", lineno);
				}
			} else
			if (1 == sscanf (line, "\"%[^\"]\" \"\"", a)) {
				if (append_osc_message(r, a, "", "")) {
					fprintf (stderr, "Failed to append/parse OSC message from line: %d\n", lineno);
				}
			} else {
				fprintf (stderr, "Invalid OSC message format. line: %d\n", lineno);
			}
		}
		else if (parser_state == StartRule) {
			r = new_rule (line);
			if (r) {
				parser_state = InRule;
			} else {
				parser_state = NoRule;
			}
		}
		else if (parser_state == InConfig) {
			if (!strncasecmp(line, "osc=", 4) && strlen(line) > 4) {
				parse_osc_addr(line + 4);
			}
			else if (!strncasecmp(line, "input=", 6) && strlen(line) > 6) {
				free (j_connect);
				j_connect = strdup(line + 6);
			}
			else if (!strncasecmp(line, "syncmode=", 9) && strlen(line) > 9) {
				parse_sync_mode(line + 9);
			}
		} else {
			fprintf (stderr, "Ignored config line: %d\n", lineno);
		}
	}

parser_end:
	if (rv) {
		fprintf (stderr, "Failed to parse config, line %d\n", lineno);
	}

	fclose(f);
	return rv;

	return 0;
}

static void dump_cfg (void) {
	int j;
	printf("\n# ----- CFG DUMP -----\n");
	printf("[config]\n");
	if (osc_dest) {
		printf("# OSC destination\n");
		printf("osc=%s:%s\n\n",
				lo_address_get_hostname(osc_dest),
				lo_address_get_port(osc_dest));
	}
	if (j_connect) {
		printf("# auto-connect to jack-midi capture port\n");
		printf("input=%s\n\n", j_connect);
	}
	printf("\n");

	for (j = 0; j < rule_count; ++j) {
		int i;
		Rule *r = &rules[j];
		printf("# rule %d\n", j);
		printf("[rule]\n0x%02x/0x%02x", r->match[0], r->mask[0]);
		if (r->len > 1) {
			printf(" 0x%02x/0x%02x", r->match[1], r->mask[1]);
		}
		if (r->len > 2) {
			printf(" 0x%02x/0x%02x", r->match[1], r->mask[1]);
		}
		printf("\n");

		const unsigned int mc = r->message_count;
		for (i = 0; i < mc; ++i) {
			int k;
			const unsigned int pl = strlen (r->msg[i].desc);

			printf("\"%s\" \"%s\"",
					r->msg[i].path, r->msg[i].desc);

			for (k = 0; k < pl; ++k) {
				printf(" \"%s\"", r->msg[i].param[k]);
			}
			printf("\n");
		}
		printf("\n");
	}
	printf("# --------------------\n");
}

/******************************************************************************
 * MIDI to OSC translation
 */

static int32_t expand_int32 (const char *tpl, MidiMessage *m) {
	assert (tpl[0] != '\0');
	if (tpl[0] != '%') {
		return atoi (tpl);
	}

	char x;
	int source[2];
	int target[2];
	if (5 == sscanf(tpl, "%%%c [%i,%i] [%i,%i]", &x, &target[0], &target[1], &source[0], &source[1])) {
		;
	} else if (3 == sscanf(tpl, "%%%c [%i,%i]", &x, &target[0], &target[1])) {
		source[0] = 0;
		source[1] = 0x7f;
	} else if (1 == sscanf(tpl, "%%%c", &x)) {
		source[0] = 0;
		source[1] = 0x7f;
		target[0] = 0;
		target[1] = 0x7f;
	} else {
		fprintf (stderr, "Invalid expression: %s\n", tpl);
		return 0;
	}

	if (source[0] >= source[1] || source[0] < 0 || source[1] > 0x7f) {
		fprintf (stderr, "Invalid Range: %s\n", tpl);
		return 0;
	}

	int val;
	switch (x) {
		case '0':
			val = m->d[0] & 0xff;
			break;
		case '1':
			val = m->d[1] & 0x7f;
			break;
		case '2':
			val = m->d[2] & 0x7f;
			break;
		case 'c':
			val = m->d[0] & 0x0f;
			// TODO use default src range 0..15
			break;
		case 's':
			val = m->d[0] & 0xf0;
			break;
		default:
			fprintf (stderr, "Invalid Placeholder: %s\n", tpl);
			return 0.f;
			break;
	}

	if (val <= source[0]) return target[0];
	if (val >= source[1]) return target[1];

	return target[0] + (val - source[0]) * (target[1] - target[0]) / (target[1] - target[0]);
}

static float expand_float (const char *tpl, MidiMessage *m) {
	assert (tpl[0] != '\0');
	if (tpl[0] != '%') {
		return atof (tpl);
	}

	char x;
	int source[2];
	float target[2];
	if (5 == sscanf(tpl, "%%%c [%f,%f] [%i,%i]", &x, &target[0], &target[1], &source[0], &source[1])) {
		;
	} else if (3 == sscanf(tpl, "%%%c [%f,%f]", &x, &target[0], &target[1])) {
		source[0] = 0;
		source[1] = 0x7f;
	} else if (1 == sscanf(tpl, "%%%c", &x)) {
		source[0] = 0;
		source[1] = 0x7f;
		target[0] = 0.f;
		target[1] = 127.f;
	} else {
		fprintf (stderr, "Invalid expression: %s\n", tpl);
		return 0.f;
	}

	if (source[0] >= source[1] || source[0] < 0 || source[1] > 0x7f) {
		fprintf (stderr, "Invalid Range: %s\n", tpl);
		return 0.f;
	}

	float val;
	switch (x) {
		case '0':
			val = m->d[0] & 0xff;
			break;
		case '1':
			val = m->d[1] & 0x7f;
			break;
		case '2':
			val = m->d[2] & 0x7f;
			break;
		case 'c':
			val = m->d[0] & 0x0f;
			break;
		case 's':
			val = m->d[0] & 0xf0;
			break;
		default:
			fprintf (stderr, "Invalid Placeholder: %s\n", tpl);
			return 0.f;
			break;
	}

	if (val <= source[0]) return target[0];
	if (val >= source[1]) return target[1];

	return target[0] + (val - source[0]) * (target[1] - target[0]) / (float)(source[1] - source[0]);
}

static void expand_and_send (Rule *r, MidiMessage *m) {
	unsigned int i,c;
	const unsigned int mc = r->message_count;

	for (i = 0; i < mc; ++i) {
		lo_message oscmsg = lo_message_new();
		if (!oscmsg) {
			fprintf (stderr, "Cannot allocate OSC Message.\n");
			continue;
		}

		int err = 0;
		for (c = 0; c < strlen(r->msg[i].desc); ++c) {
			switch (r->msg[i].desc[c]) {
				case LO_INT32:
					err |= lo_message_add_int32 (oscmsg, expand_int32 (r->msg[i].param[c], m));
					break;
				case LO_FLOAT:
					err |= lo_message_add_float (oscmsg, expand_float (r->msg[i].param[c], m));
					break;
				case LO_STRING:
					err |= lo_message_add_string (oscmsg, r->msg[i].param[c]);
					break;
				default:
					fprintf(stderr, "Failed to expand OSC parameter '%c'.\n", r->msg[i].desc[c]);
					err = 1;
					break;
			}
		}
		if (err != 0) {
			fprintf(stderr, "Failed to construct OSC message\n");
			lo_message_free (oscmsg);
			continue;;
		}

		if (want_verbose > 1) {
			printf("TX: %s ", r->msg[i].path);
			lo_message_pp(oscmsg);
		}

		if (-1 == lo_send_message (osc_dest, r->msg[i].path, oscmsg)) {
			fprintf(stderr, "Failed to send OSC message '%s'.\n", r->msg[i].path);
		}
		lo_message_free (oscmsg);
	}
}

/******************************************************************************
 * main application code
 */

#ifndef _WIN32
static void wearedone (int sig) {
	fprintf (stderr,"caught signal - shutting down.\n");
	run = Terminate;
	pthread_cond_signal (&data_ready);
	signal (SIGHUP, SIG_DFL);
	signal (SIGINT, SIG_DFL);
}
#endif

static struct option const long_options[] =
{
	{"config", required_argument, 0, 'c'},
	{"help", no_argument, 0, 'h'},
	{"input", required_argument, 0, 'i'},
	{"osc", required_argument, 0, 'o'},
	{"syncmode", required_argument, 0, 's'},
	{"verbose", no_argument, 0, 'v'},
	{"version", no_argument, 0, 'V'},
	{NULL, 0, NULL, 0}
};

static void usage (int status) {
	printf ("jackmidi2osc - JACK MIDI to OSC.\n\n");
	printf ("Usage: jackmidi2osc [ OPTIONS ]\n\n");
	printf ("Options:\n\
  -c <file>, --config <file>\n\
                        specify configuration file\n\
  -h, --help            display this help and exit\n\
  -i <port-name>, --input <port-name>\n\
                        auto-connect to given jack-midi capture port\n\
  -o <addr>, --osc <addr>\n\
                        set OSC destination address\n\
                        as 'host:port' or simply port-number\n\
                        (defaults to localhost:3819)\n\
  -s <mode>, --syncmode <mode>\n\
                        OSC event timing. Mode is one of 'Immediate',\n\
                        'Absolute', 'Relative' (default: 'Immediate')\n\
  -v, --verbose         increase verbosity (can be used twice)\n\
  -V, --version         print version information and exit\n\
\n");
/*                                  longest help text w/80 chars per line ---->|\n" */
	printf ("\n\
A configurable tool to read midi events from a JACK MIDI port and trigger OSC\n\
messages.\n\
\n\
The main use-case is to perform complex actions with a simple MIDI-event.\n\
e.g set Ardour-mixer scenes (mute, gain, plugin-settings) with a single button press.\n\
jackmidi2osc also facilitates to translating MIDI note and CC events to OSC in realtime.\n\
\n\
See the example configuration file for further explanation.\n\
\n\
Configuration Files:\n\
By default jackmidi2osc reads $XDG_CONFIG_HOME/jackmidi2osc/default.cfg\n\
on startup if the file exists.\n\
\n\
Sync Modes:\n\
 'Immediate'   send events as soon as possible. Ignore event time.\n\
               All events from one jack cycle are sent successively\n\
 'Absolute'    Use absolute event time (audio clock). Future events are\n\
               queued, past events are sent immediatley.\n\
               Depending on network I/O, events near the beginning of\n\
               a JACK cycle may be in the 'past' (compared to absolute\n\
               time) and hence the OSC stream is jittery\n\
 'Relative'    use relative time (audio clock) between MIDI events\n\
               with one cycle latency.\n\
               Compared to 'absolute' this mode has smaller jitter and\n\
               always retains the timing.\n\
\n");
	printf ("Report bugs to Robin Gareus <robin@gareus.org>\n"
	        "Website and manual: <https://github.com/x42/jackmidi2osc>\n"
			);
	exit (status);
}

static int decode_switches (int argc, char **argv) {
	int c;

	while ((c = getopt_long (argc, argv,
					"c:" /* configfile */
					"h"  /* help */
					"i:" /* MIDI port */
					"o:" /* osc dest */
					"s:" /* sync-mode */
					"v"  /* verbose */
					"V", /* version */
					long_options, (int *) 0)) != EOF) {
		switch (c) {
			case 'c':
				free(cfgfile);
				cfgfile = strdup (optarg);
				break;
			case 'i':
				free (j_connect);
				j_connect = strdup (optarg);
				break;
			case 'o':
				if (parse_osc_addr (optarg)) {
					usage (EXIT_FAILURE);
				}
				break;
			case 's':
				if (parse_sync_mode (optarg)) {
					fprintf (stderr, "Invalid sync mode option given\n");
					usage (EXIT_FAILURE);
				}
				break;
			case 'v':
				++want_verbose;
				break;
			case 'V':
				printf ("jackmidi2osc version %s\n\n", VERSION);
				printf ("Copyright (C) GPL 2013,2015 Robin Gareus <robin@gareus.org>\n");
				exit (0);
			case 'h':
				usage (0);
			default:
				usage (EXIT_FAILURE);
		}
	}
	return optind;
}

static int testfile (char *filename) {
	struct stat s;
	int result= stat(filename, &s);
	if (result != 0) return 0;
	if (S_ISREG(s.st_mode)) return 1;
	return(0);
}

static void user_config_file (const char *fn) {
	char filename[PATH_MAX];
#ifdef _WIN32
	const char * homedrive = getenv("HOMEDRIVE");
	const char * homepath = getenv("HOMEPATH");
	if (homedrive && homepath && (strlen(homedrive) + strlen(homepath) + strlen(fn) + 29) < PATH_MAX) {
		sprintf(filename, "%s%s\\Local Settings\\jackmidi2osc\\%s", homedrive, homepath, fn);
		if (testfile(filename)) read_config(filename);
	}
#else // unices - use XDG_CONFIG_HOME
	const char *xdg = getenv("XDG_CONFIG_HOME");
	const char *home = getenv("HOME");
	if (xdg && (strlen(xdg) + strlen(fn) + 15) < PATH_MAX) {
		sprintf(filename, "%s/jackmidi2osc/%s", xdg, fn);
		if (testfile(filename)) read_config(filename);
	}
	// XDG_CONFIG_HOME fallback
#ifdef __APPLE__
	if (!xdg && home && (strlen(home) + strlen(fn) + 35) < PATH_MAX) {
		sprintf(filename, "%s/Library/Preferences/jackmidi2osc/%s", home, fn);
		if (testfile(filename)) read_config(filename);
	}
#else
	if (!xdg && home && (strlen(home) + strlen(fn) + 23) < PATH_MAX) {
		sprintf(filename, "%s/.config/jackmidi2osc/%s", home, fn);
		if (testfile(filename)) read_config(filename);
	}
#endif
#endif
}

int main (int argc, char ** argv) {

	user_config_file ("default.cfg");

	decode_switches (argc, argv);

	if (optind > argc) {
		usage (EXIT_FAILURE);
	}

	if (cfgfile && read_config (cfgfile)) {
		goto out;
	}

	if (rule_count == 0) {
		fprintf (stderr, "No MIDI-> OSC Rules configured\n");
		goto out;
	}

	if (init_jack ("jackmidi2osc")) {
		goto out;
	}

	if (jack_portsetup ()) {
		goto out;
	}

	if (!osc_dest) {
		osc_dest = lo_address_new (NULL, "3819");
	}

	if (want_verbose > 0) {
		printf ("Parsed %d rules\n", rule_count);
		char *url = lo_address_get_url(osc_dest);
		printf ("Sending Messages to %s\n", url);
		free(url);
		if (want_verbose > 1) {
			dump_cfg();
		}
	}

	rb = jack_ringbuffer_create (RINGBUF_SIZE * sizeof (MidiMessage));

	if (!rb) {
		fprintf (stderr, "Cannot allocate rinbuffer..\n");
		goto out;
	}

#ifndef _WIN32
	if (mlockall (MCL_CURRENT | MCL_FUTURE)) {
		fprintf (stderr, "Warning: Cannot lock memory.\n");
	}
#endif

	if (jack_activate (j_client)) {
		fprintf (stderr, "cannot activate client.\n");
		goto out;
	}

	if (inport_connect (j_connect)) {
		goto out;
	}

#ifndef _WIN32
	signal (SIGHUP, wearedone);
	signal (SIGINT, wearedone);
#endif

	pthread_mutex_lock (&msg_thread_lock);

	const jack_nframes_t deadzone = (sync_mode == SyncImmediate) ? 0 : ceil (0.0005 * samplerate); // .5ms
	assert (deadzone >= 0);

	/* all systems go */
	run = Running;
	printf("Press Ctrl+C to terminate\n");

	while (run != Terminate && j_client) {
		int i,j;
		const int mqlen = jack_ringbuffer_read_space (rb) / sizeof (MidiMessage);
		for (i = 0; i < mqlen; ++i) {
			MidiMessage mmsg;
			jack_ringbuffer_read (rb, (char*) &mmsg, sizeof (MidiMessage));

			if (want_verbose > 1) {
				printf ("RX MIDI: [0x%02x 0x%02x 0x%02x] @%"PRIu32"\n",
						(uint8_t)mmsg.d[0], (uint8_t)mmsg.d[1], (uint8_t)mmsg.d[2], mmsg.tme
						);
			}

			if (deadzone > 0) {
				jack_nframes_t now = jack_frame_time (j_client);
				//printf("NOW:                      @%"PRIu32"\n", now);
				while (run != Terminate && j_client && now < mmsg.tme + deadzone) {
					if ((mmsg.tme & 0x8000000) ^ (now & 0x8000000)) {
						break; // handle 32bit roll-over
					}
					usleep((mmsg.tme + deadzone - now) * 1e6 / samplerate);
					now = jack_frame_time (j_client);
				}
				if (run == Terminate) {
					break;
				}
			}

			for (j = 0; j < rule_count; ++j) {
				Rule *r = &rules[j];
				if (    (r->len == 0 || r->len == mmsg.len)
				     && (                (mmsg.d[0] & r->mask[0]) == r->match[0])
				     && (mmsg.len < 2 || (mmsg.d[1] & r->mask[1]) == r->match[1])
				     && (mmsg.len < 3 || (mmsg.d[2] & r->mask[2]) == r->match[2])
				   )
				{
					if (want_verbose > 1) {
						printf("       | Rule #%d -> %d osc msg(s)\n", j, r->message_count);
					}
					expand_and_send (r, &mmsg);
				}
			}
		}
		fflush (stdout);
		pthread_cond_wait (&data_ready, &msg_thread_lock);
	}

	pthread_mutex_unlock (&msg_thread_lock);

	if (want_verbose > 0) {
		printf ("\nDropped Messages: %d\n", dropped_messages);
	}

out:

	cleanup ();
	return 0;
}
/* vi:set ts=2 sts=2 sw=2: */
