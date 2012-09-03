#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <caputils/caputils.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <csignal>
#include <getopt.h>
#include <functional>

#include "extract.hpp"

static const char* iface = NULL;
const char* program_name = NULL;

static void handle_sigint(int signum){
	if ( !keep_running ){
		fprintf(stderr, "\rGot SIGINT again, terminating.\n");
		abort();
	}
	fprintf(stderr, "\rAborting capture.\n");
	keep_running = false;
}

static double my_round (double value){
	static const double bias = 0.0005;
	return (floor(value + bias));
}

class Bin {
public:
	Bin(int level, int timescale, int moments, Bin* next = nullptr)
		: next(next)
		, level(level)
		, timescale(timescale)
		, num_moments(moments)
		, accumulator(nullptr)
		, previous(0.0)
		, counter(0) {

		setup_accumulator();
	}

	~Bin(){
		delete [] accumulator;
		delete next;
	}

	void setup_accumulator(){
		accumulator = new double[num_moments];
		for ( int i = 0; i < num_moments; i++ ){
			accumulator[i] = 0.0;
		}
	}

	/**
	 * Called for each value.
	 */
	void feed(double value){
		for ( int i = 0; i < num_moments; i++ ){
			accumulator[i] += pow(value, (double)(i+1));
		}

		if ( ++counter % timescale == 0 ){
			sample();
		}
	}

	/**
	 * Called when enough datapoints was gathered.
	 */
	void sample(){
		const double mean = (accumulator[0] - previous) / timescale;
		previous = accumulator[0];

		if ( !next ){
			next = new Bin(level+1, timescale, num_moments);
		}
		next->feed(mean);
	}

	void recursive_visit(std::function<void(Bin*)> callback){
		callback(this);
		if ( next ){
			next->recursive_visit(callback);
		}
	}

	void recursive_visit(std::function<void(const Bin*)> callback) const {
		callback(this);
		if ( next ){
			((const Bin*)next)->recursive_visit(callback);
		}
	}

private:
	friend class Timescale;
	friend class DefaultOutput;
	friend class CSVOutput;

	Bin* next;
	const int level;
	const int timescale;
	const int num_moments;
	double* accumulator;
	double previous;
	int counter;
};

class Output {
public:
	virtual void write_output(const Bin* bin, int timescale, int num_moments, double sampleFrequency, double tSample) = 0;
};

class DefaultOutput: public Output {
public:
	virtual void write_output(const Bin* bin, int timescale, int num_moments, double sampleFrequency, double tSample){
		int width[num_moments];
		for ( int i = 0; i < num_moments; i++ ){
			width[i] = str_width_for_moment(bin, i);
		}

		fprintf(stdout, "sampleFrequency: %.2fHz\n", sampleFrequency);
		fprintf(stdout, "tSample:         %fs\n", tSample);
		fprintf(stdout, "timescale:       %d\n", timescale);
		fprintf(stdout, "\n");

		fprintf(stdout, "Tscale ");
		for ( int i = 0; i < num_moments; i++ ){
			fprintf(stdout, "%*s%d ", width[i], "M", i+1);
		}
		fputc('\n', stdout);

		bin->recursive_visit([&](const Bin* cur){
			fprintf(stdout, "%-6d ", (int)pow((double)cur->timescale, (double)cur->level));
			for ( int i = 0; i < num_moments; i++ ){
				fprintf(stdout, "%*g ", width[i]+1, cur->accumulator[i] / cur->counter);
			}
			fputc('\n', stdout);
		});
	}

private:
	size_t str_width_for_moment(const Bin* bin, int index){
		size_t max = 0;
		char buf[64];
		bin->recursive_visit([&](const Bin* cur){
			size_t width = snprintf(buf, sizeof(buf), "%g", cur->accumulator[index] / cur->counter);
			max = width > max ? width : max;
		});
		return max;
	}
};

class CSVOutput: public Output {
public:
	CSVOutput(char delimiter, bool show_header)
		: delimiter(delimiter)
		, show_header(show_header){

	}

	virtual void write_output(const Bin* bin, int timescale, int num_moments, double sampleFrequency, double tSample){
		if ( show_header ){
			fprintf(stdout, "\"Tscale (%dx, %.2fHz)\"", timescale, sampleFrequency);
			for ( int i = 0; i < num_moments; i++ ){
				fprintf(stdout, "%c\"M%d\"", delimiter, i+1);
			}
			fprintf(stdout, "\n");
		}

		bin->recursive_visit([&](const Bin* cur){
			printf("%d", (int)pow((double)cur->timescale, (double)cur->level));
			for ( int i = 0; i < num_moments; i++ ){
				printf("%c%f", delimiter, cur->accumulator[i] / cur->counter);
			}
			putchar('\n');
		});
	};

private:
	char delimiter;
	bool show_header;
};

class Timescale: public Extractor {
public:
	Timescale()
		: Extractor()
		, num_moments(3)
		, timescale(10)
		, bin(nullptr)
		, bits(0.0) {

		set_formatter(FORMAT_DEFAULT);
	}

	void set_timescale(int timescale){
		this->timescale = timescale;
	}

	void set_moments(int moments){
		num_moments = moments;
	}

	virtual void set_formatter(enum Formatter format){
		switch (format){
		  case FORMAT_DEFAULT: output = new DefaultOutput; break;
		  case FORMAT_CSV:     output = new CSVOutput(';', false); break;
		  case FORMAT_TSV:     output = new CSVOutput('\t', false); break;
		  case FORMAT_MATLAB:  output = new CSVOutput('\t', true); break;
		}
	}

	using Extractor::set_formatter;

	virtual void reset(){
		Extractor::reset();

		delete bin;
		bin = new Bin(0, timescale, num_moments);
		bits = 0.0;
	}

protected:
	virtual void write_trailer(int index){
		output->write_output(bin, timescale, num_moments, sampleFrequency, to_double(tSample));
	}

	virtual void write_sample(double t){
		const double bitrate = my_round(bits / to_double(tSample));
		bin->feed(bitrate);
		bits = 0.0;
	}

	virtual void accumulate(qd_real fraction, unsigned long packet_bits, const cap_head* cp, int counter){
		bits += my_round(to_double(fraction) * packet_bits);
	}

private:
	Output* output;
	int num_moments;
	int timescale;
	Bin* bin;
	double bits;
};

static const char* short_options = "p:i:q:m:f:t:n:h";
static struct option long_options[]= {
	{"packets",          required_argument, 0, 'p'},
	{"iface",            required_argument, 0, 'i'},
	{"level",            required_argument, 0, 'q'},
	{"sampleFrequency",  required_argument, 0, 'm'},
	{"format",           required_argument, 0, 'f'},
	{"timescale",        required_argument, 0, 't'},
	{"moments",          required_argument, 0, 'n'},
	{"help",             no_argument,       0, 'h'},
	{0, 0, 0, 0} /* sentinel */
};

static void show_usage(void){
	printf("%s-" VERSION " (libcap_utils-%s)\n", program_name, caputils_version(NULL));
	printf("(C) 2012 David Sveningsson <david.sveningsson@bth.se>\n");
	printf("Usage: %s [OPTIONS] STREAM\n", program_name);
	printf("  -i, --iface                 For ethernet-based streams, this is the interface to listen\n"
	       "                              on. For other streams it is ignored.\n"
	       "  -m, --sampleFrequency       Sampling frequency in Hz. Valid prefixes are 'k', 'm' and 'g'.\n"
	       "  -q, --level 		            Level to calculate bitrate {physical (default), link, network, transport and application}\n"
	       "                              At level N , payload of particular layer is only considered, use filters to select particular streams.\n"
	       "                              To calculate the bitrate at physical , use physical layer, Consider for Network layer use [-q network]\n"
	       "                              It shall contain transport protocol header + payload\n"
	       "                                - link: all bits captured at physical level, i.e link + network + transport + application\n"
	       "                                - network: payload field at link layer , network + transport + application\n"
	       "                                - transport: payload at network  layer, transport + application\n"
	       "                                - application: The payload field at transport leve , ie.application\n"
	       "                              Default is link\n"
	       "  -p, --packets=N             Stop after N packets.\n"
	       "  -f, --format=FORMAT         Set a specific output format. See below for list of supported formats.\n"
	       "  -t, --timescale=SCALE       Set timescale [default: 10].\n"
	       "  -n, --moments=MOMENTS       Show N moments [default: 3].\n"
	       "  -h, --help                  This text.\n\n");

	output_format_list();
	filter_from_argv_usage();
}

int main(int argc, char **argv){
	/* extract program name from path. e.g. /path/to/MArCd -> MArCd */
	const char* separator = strrchr(argv[0], '/');
	if ( separator ){
		program_name = separator + 1;
	} else {
		program_name = argv[0];
	}

	struct filter filter;
	if ( filter_from_argv(&argc, argv, &filter) != 0 ){
		return 0; /* error already shown */
	}

	Timescale app;

	int op, option_index = -1;
	while ( (op = getopt_long(argc, argv, short_options, long_options, &option_index)) != -1 ){
		switch (op){
		case 0:   /* long opt */
		case '?': /* unknown opt */
			break;

		case 'f': /* --format */
			app.set_formatter(optarg);
			break;

		case 'p':
			app.set_max_packets(atoi(optarg));
			break;

		case 'm' : /* --sampleFrequency */
			app.set_sampling_frequency(optarg);
			break;

		case 'q': /* --level */
			app.set_extraction_level(optarg);
			break;

		case 'l': /* --link */
			app.set_link_capacity(optarg);
			break;

		case 'i':
			iface = optarg;
			break;

		case 't': /* --timescale */
			app.set_timescale(atoi(optarg));
			break;

		case 'n': /* --moments */
			app.set_moments(atoi(optarg));
			break;

		case 'h':
			show_usage();
			return 0;

		default:
			fprintf (stderr, "%s: ?? getopt returned character code 0%o ??\n", program_name, op);
		}
	}

	/* handle C-c */
	signal(SIGINT, handle_sigint);

	int ret;

	/* Open stream(s) */
	struct stream* stream;
	if ( (ret=stream_from_getopt(&stream, argv, optind, argc, iface, "-", program_name, 0)) != 0 ) {
		return ret; /* Error already shown */
	}

	app.reset();
	app.process_stream(stream, &filter);

	/* Release resources */
	stream_close(stream);
	filter_close(&filter);

	return 0;
}
