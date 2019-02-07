/*
LDPC testbench
Copyright 2018 Ahmet Inan <xdsopl@gmail.com>

Transformed into external decoder for third-party applications
Copyright 2019 <pabr@pabr.org>
*/

#include <stdlib.h>
#include <unistd.h>
#include <iostream>
#include <iomanip>
#include <random>
#include <cmath>
#include <cassert>
#include <chrono>
#include <cstring>
#include <algorithm>
#include <functional>
#include "testbench.hh"
#include "encoder.hh"
#include "algorithms.hh"
#include "interleaver.hh"
#include "modulation.hh"

#if 0
#include "flooding_decoder.hh"
static const int TRIALS = 50;
#else
#include "layered_decoder.hh"
static const int TRIALS = 25;
#endif

LDPCInterface *create_ldpc(char *standard, char prefix, int number);
Interleaver<code_type> *create_interleaver(char *modulation, char *standard, char prefix, int number);
ModulationInterface<complex_type, code_type> *create_modulation(char *name, int len);

void fail(const char *msg) {
	std::cerr << "** plugin: " << msg << std::endl;
	exit(1);
}
void fatal(const char *msg) {
	fprintf(stderr, "** plugin: ");
	perror(msg);
	exit(1);
}

void usage(const char *name, FILE *f, int c, const char *info=NULL) {
	fprintf(f, "Usage: %s [--standard DVB-S2] --modcod INT [--shortframes]  < FECFRAMES.int8  > FECFRAMES.int8\n", name);
	if ( info ) fprintf(f, "** Error while processing '%s'\n", info);
	exit(c);
}

int main(int argc, char **argv)
{
	const char *standard = "DVB-S2";
	int modcod = -1;
	bool shortframes = false;
	for ( int i=1; i<argc; ++i ) {
		if      ( ! strcmp(argv[i],"--standard") && i+1<argc ) standard = argv[++i];
		else if ( ! strcmp(argv[i],"--modcod") && i+1<argc )   modcod = atoi(argv[++i]);
		else if ( ! strcmp(argv[i],"--shortframes") )          shortframes = true;
		else if ( ! strcmp(argv[i],"-h") )                     usage(argv[0], stdout, 0);
		else                                                   usage(argv[0], stderr, 1, argv[i]);
	}

	if ( strcmp(standard, "DVB-S2") ) fail("Only DVB-S2 is supported.");
	if ( modcod<0 || modcod>31 ) usage(argv[0], stderr, 1);

	typedef NormalUpdate<simd_type> update_type;
	//typedef SelfCorrectedUpdate<simd_type> update_type;

	//typedef MinSumAlgorithm<simd_type, update_type> algorithm_type;
	//typedef OffsetMinSumAlgorithm<simd_type, update_type, FACTOR> algorithm_type;
	typedef MinSumCAlgorithm<simd_type, update_type, FACTOR> algorithm_type;
	//typedef LogDomainSPA<simd_type, update_type> algorithm_type;
	//typedef LambdaMinAlgorithm<simd_type, update_type, 3> algorithm_type;
	//typedef SumProductAlgorithm<simd_type, update_type> algorithm_type;

	LDPCEncoder<code_type> encode;
	LDPCDecoder<simd_type, algorithm_type> decode;

	// DVB-S2 MODCOD definitions
	static const char *mc_constnames[32] = {
	  0,"QPSK","QPSK","QPSK","QPSK","QPSK","QPSK","QPSK",
	  "QPSK","QPSK","QPSK","QPSK","8PSK","8PSK","8PSK","8PSK",
	  "8PSK","8PSK","16APSK","16APSK","16APSK","16APSK","16APSK","16APSK",
	  "32APSK","32APSK","32APSK","32APSK","32APSK",0,0
	};
	static const char *mc_tabnames[2][32] = {
	  { // Normal frames
	    0,"B1","B2","B3","B4","B5","B6","B7",
	    "B8","B9","B10","B11","B5","B6","B7","B9",
	    "B10","B11","B6","B7","B8","B9","B10","B11",
	    "B7","B8","B8","B10","B11",0,0,0
	  },
	  { // Short frames
	    0,"C1","C2","C3","C4","C5","C6","C7",
	    "C8","C9","C10",0,"C5","C6","C7","C9",
	    "C10",0,"C6","C7","C8","C9","C10",0,
	    "C7","C8","C8","C10",0,0,0,0
	  }
	};

	const char *tabname = mc_tabnames[shortframes][modcod];
	if ( ! tabname ) fail("unsupported modcod");
	LDPCInterface *ldpc = create_ldpc((char*)"S2", tabname[0], atoi(tabname+1));
	if (!ldpc) {
		std::cerr << "no such table!" << std::endl;
		return -1;
	}
	const int CODE_LEN = ldpc->code_len();
	const int DATA_LEN = ldpc->data_len();

	encode.init(ldpc);
	decode.init(ldpc);
	
	const char *constname = mc_constnames[modcod];
	if ( ! constname ) fail("unsupported modcod");
	ModulationInterface<complex_type, code_type> *mod = create_modulation((char*)constname, CODE_LEN);
	if (!mod) {
		std::cerr << "no such modulation!" << std::endl;
		return -1;
	}
	const int MOD_BITS = mod->bits();
	assert(CODE_LEN % MOD_BITS == 0);

	int BLOCKS = 32;
	code_type *code = new code_type[BLOCKS * CODE_LEN];
	void *aligned_buffer = aligned_alloc(sizeof(simd_type), sizeof(simd_type) * CODE_LEN);
	simd_type *simd = reinterpret_cast<simd_type *>(aligned_buffer);

	// Expect LLR values in int8_t format.
	if ( sizeof(code_type) != 1 ) fail("Bug: Unsupported code_type");

	while ( true ) {
		ssize_t iosize = BLOCKS * CODE_LEN * sizeof(*code);
		for ( ssize_t pos=0; pos<iosize; ) {
			int nr = read(0, code+pos, iosize-pos);
			if ( ! nr ) exit(0);
			if ( nr < 0 ) fatal("read");
			pos += nr;
		}
	  
		int iterations = 0;
		int num_decodes = 0;
		for (int j = 0; j < BLOCKS; j += SIMD_WIDTH) {
			int blocks = j + SIMD_WIDTH > BLOCKS ? BLOCKS - j : SIMD_WIDTH;
			for (int n = 0; n < blocks; ++n)
				for (int i = 0; i < CODE_LEN; ++i)
					reinterpret_cast<code_type *>(simd+i)[n] = code[(j+n)*CODE_LEN+i];
			int trials = TRIALS;
			int count = decode(simd, simd + DATA_LEN, trials, blocks);
			++num_decodes;
			for (int n = 0; n < blocks; ++n)
				for (int i = 0; i < CODE_LEN; ++i)
					code[(j+n)*CODE_LEN+i] = reinterpret_cast<code_type *>(simd+i)[n];
			if (count < 0) {
				iterations += blocks * trials;
				std::cerr << "decoder failed at converging to a code word!" << std::endl;
			} else {
				iterations += blocks * (trials - count);
			}
		}

		for (int i = 0; i < BLOCKS * CODE_LEN; ++i)
			assert(!std::isnan(code[i]));

		ssize_t nw = write(1, code, iosize);
		if ( ! nw ) break;
		if ( nw < 0 ) fatal("write");
		if ( nw != iosize ) fail("partial write");
	}  // main loop


	delete ldpc;
	delete mod;

	free(aligned_buffer);
	delete[] code;

	return 0;
}