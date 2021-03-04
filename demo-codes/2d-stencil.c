/*
 * Copyright (c) 2012 Torsten Hoefler. All rights reserved.
 *
 * Author(s): Torsten Hoefler <htor@illinois.edu>
 *
 */

//TODO das so umschreiben, dass kiene License verletzt wird
// code aus: https://www.mcs.anl.gov/~thakur/sc14-mpi-tutorial/
#include "mpi.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "correctness-checking-partitioned-impl.h"

// row-major order
#define ind(i,j) (j)*(bx+2)+(i)

struct bmpfile_magic {
	unsigned char magic[2];
};

struct bmpfile_header {
	uint32_t filesz;
	uint16_t creator1;
	uint16_t creator2;
	uint32_t bmp_offset;
};

struct bmpinfo_header {
	uint32_t header_sz;
	int32_t width;
	int32_t height;
	uint16_t nplanes;
	uint16_t bitspp;
	uint32_t compress_type;
	uint32_t bmp_bytesz;
	int32_t hres;
	int32_t vres;
	uint32_t ncolors;
	uint32_t nimpcolors;
};

void printarr_par(int iter, double *array, int size, int px, int py, int rx,
		int ry, int bx, int by, int offx, int offy, MPI_Comm comm) {

	int myrank;
	MPI_Comm_rank(comm, &myrank);
	MPI_File fh;

	char fname[128];
	snprintf(fname, 128, "./output-%i.bmp", iter);

	MPI_File_open(comm, fname,
	MPI_MODE_SEQUENTIAL | MPI_MODE_CREATE | MPI_MODE_WRONLY,
	MPI_INFO_NULL, &fh);

	if (myrank == 0) {

		struct bmpfile_magic magic;
		struct bmpfile_header header;
		struct bmpinfo_header bmpinfo;

		magic.magic[0] = 0x42;
		magic.magic[1] = 0x4D;

		MPI_File_write_shared(fh, &magic, sizeof(struct bmpfile_magic),
		MPI_BYTE, MPI_STATUSES_IGNORE);

		header.filesz = sizeof(struct bmpfile_magic)
				+ sizeof(struct bmpfile_header) + sizeof(struct bmpinfo_header)
				+ size * (size * 3 + size * 3 % 4);
		header.creator1 = 0xFE;
		header.creator1 = 0xFE;
		header.bmp_offset = sizeof(struct bmpfile_magic)
				+ sizeof(struct bmpfile_header) + sizeof(struct bmpinfo_header);

		MPI_File_write_shared(fh, &header, sizeof(struct bmpfile_header),
		MPI_BYTE, MPI_STATUSES_IGNORE);

		bmpinfo.header_sz = sizeof(struct bmpinfo_header);
		bmpinfo.width = size;
		bmpinfo.height = size;
		bmpinfo.nplanes = 1;
		bmpinfo.bitspp = 24;
		bmpinfo.compress_type = 0;
		bmpinfo.bmp_bytesz = size * (size * 3 + size * 3 % 4);
		bmpinfo.hres = size;
		bmpinfo.vres = size;
		bmpinfo.ncolors = 0;
		bmpinfo.nimpcolors = 0;

		MPI_File_write_shared(fh, &bmpinfo, sizeof(struct bmpinfo_header),
		MPI_BYTE, MPI_STATUSES_IGNORE);

	}

	int xcnt, ycnt, my_xcnt, my_ycnt;
	int i;

	int linesize = bx * 3;
	int padding = 0;
	if (((rx + 1) % px) == 0)
		padding = size * 3 % 4;
	char *myline = (char*) malloc(linesize + padding);

	my_xcnt = 0;
	my_ycnt = 0;
	xcnt = 0;
	ycnt = size;

	while (ycnt >= 0) {
		MPI_Barrier(comm);
		if ((xcnt == offx) && (ycnt >= offy) && (ycnt < offy + by)) {
			for (i = 0; i < linesize; i += 3) {
				int rgb;
				if (i / 3 > bx)
					rgb = 0;
				else
					rgb = (int) round(255.0 * array[ind(i / 3, by - my_ycnt)]);
				if ((i == 0) || (i / 3 == bx - 1) || (my_ycnt == 0)
						|| (my_ycnt == by - 1))
					rgb = 255;
				if (rgb > 255)
					rgb = 255;
				myline[i + 0] = 0;
				myline[i + 1] = 0;
				myline[i + 2] = rgb;
			}
			my_xcnt += bx;
			my_ycnt++;
			MPI_File_write_shared(fh, myline, linesize + padding, MPI_BYTE,
			MPI_STATUSES_IGNORE);
		}
		xcnt += bx;
		if (xcnt >= size) {
			xcnt = 0;
			ycnt--;
		}
	}

	MPI_File_close(&fh);
}

int main(int argc, char **argv) {

	MPI_Init(&argc, &argv);
	int r, p;
	MPI_Comm comm = MPI_COMM_WORLD;
	MPI_Comm_rank(comm, &r);
	MPI_Comm_size(comm, &p);
	int n, energy, niters, px, py;

	if (r == 0) {
		// argument checking
		if (argc < 2) {
			// default params
			n = 40;
			energy = 10;
			niters = 10;
			px = 2;
			py = 2;
		} else {

			if (argc < 6) {
				if (!r)
					printf(
							"usage: stencil_mpi <n> <energy> <niters> <px> <py>\n");
				MPI_Finalize();
				exit(1);
			}

			n = atoi(argv[1]); // nxn grid
			energy = atoi(argv[2]); // energy to be injected per iteration
			niters = atoi(argv[3]); // number of iterations
			px = atoi(argv[4]); // 1st dim processes
			py = atoi(argv[5]); // 2nd dim processes
		}
		if (px * py != p)
			MPI_Abort(comm, 1); // abort if px or py are wrong
		if (n % py != 0)
			MPI_Abort(comm, 2); // abort px needs to divide n
		if (n % px != 0)
			MPI_Abort(comm, 3); // abort py needs to divide n

		// distribute arguments
		int args[5] = { n, energy, niters, px, py };
		MPI_Bcast(args, 5, MPI_INT, 0, comm);
	} else {
		int args[5];
		MPI_Bcast(args, 5, MPI_INT, 0, comm);
		n = args[0];
		energy = args[1];
		niters = args[2];
		px = args[3];
		py = args[4];
	}

	// determine my coordinates (x,y) -- r=x*a+y in the 2d processor array
	int rx = r % px;
	int ry = r / px;
	// determine my four neighbors
	int north = (ry - 1) * px + rx;
	if (ry - 1 < 0)
		north = MPI_PROC_NULL;
	int south = (ry + 1) * px + rx;
	if (ry + 1 >= py)
		south = MPI_PROC_NULL;
	int west = ry * px + rx - 1;
	if (rx - 1 < 0)
		west = MPI_PROC_NULL;
	int east = ry * px + rx + 1;
	if (rx + 1 >= px)
		east = MPI_PROC_NULL;
	// decompose the domain
	int bx = n / px; // block size in x
	int by = n / py; // block size in y
	int offx = rx * bx; // offset in x
	int offy = ry * by; // offset in y

	//printf("%i (%i,%i) - w: %i, e: %i, n: %i, s: %i\n", r, ry,rx,west,east,north,south);

	// allocate two work arrays
	double *aold = (double*) calloc(1, (bx + 2) * (by + 2) * sizeof(double)); // 1-wide halo zones!
	double *anew = (double*) calloc(1, (bx + 2) * (by + 2) * sizeof(double)); // 1-wide halo zones!
	double *tmp;

	// initialize three heat sources
#define nsources 3
	int sources[nsources][2] = { { n / 2, n / 2 }, { n / 3, n / 3 }, { n * 4
			/ 5, n * 8 / 9 } };
	int locnsources = 0; // number of sources in my area
	int locsources[nsources][2]; // sources local to my rank
	for (int i = 0; i < nsources; ++i) { // determine which sources are in my patch
		int locx = sources[i][0] - offx;
		int locy = sources[i][1] - offy;
		if (locx >= 0 && locx < bx && locy >= 0 && locy < by) {
			locsources[locnsources][0] = locx + 1; // offset by halo zone
			locsources[locnsources][1] = locy + 1; // offset by halo zone
			locnsources++;
		}
	}

	double t = -MPI_Wtime(); // take time
	// create north-south datatype
	MPI_Datatype north_south_type;
	MPI_Type_contiguous(bx, MPI_DOUBLE, &north_south_type);
	MPI_Type_commit(&north_south_type);
	// create east-west type
	MPI_Datatype east_west_type;
	MPI_Type_vector(by, 1, bx + 2, MPI_DOUBLE, &east_west_type);
	MPI_Type_commit(&east_west_type);

	MPI_Aint t_size = 0;
	MPI_Type_extent(north_south_type, &t_size);
	printf("North-South size:%d\n", t_size);
	MPI_Type_extent(east_west_type, &t_size);
	printf("East-West size:%d\n", t_size);

	double heat; // total heat in system
	for (int iter = 0; iter < niters; ++iter) {
		// refresh heat sources
		// introduce new heat
		for (int i = 0; i < locnsources; ++i) {
			aold[ind(locsources[i][0], locsources[i][1])] += energy; // heat source
		}

		// update grid points
		heat = 0.0;

//#pragma omp parallel for reduction(+:heat)
//#pragma omp parallel for
#pragma omp parallel for firstprivate(anew,aold) schedule(static,1000)
		for (int j = 1; j < by + 1; ++j) {
			for (int i = 1; i < bx + 1; ++i) {
				anew[ind(i, j)] = aold[ind(i, j)] / 2.0
						+ (aold[ind(i - 1, j)] + aold[ind(i + 1, j)]
								+ aold[ind(i, j - 1)] + aold[ind(i, j + 1)])
								/ 4.0 / 2.0;
				heat += anew[ind(i, j)];
			}
		}

		// exchange data with neighbors
		MPI_Request reqs[4];
//TODO Problem: irecv captures the send buffer
		// via static analysis only we are not able to prove send and recv buffer to be differen5t
		MPI_Irecv(&anew[ind(1, 0)] /* north */, 1, north_south_type, north, 9,
				comm, &reqs[0]);
		MPI_Irecv(&anew[ind(1, by + 1)] /* south */, 1, north_south_type, south,
				9, comm, &reqs[1]);
		MPI_Irecv(&anew[ind(bx + 1, 1)] /* west */, 1, east_west_type, east, 9,
				comm, &reqs[2]);
		MPI_Irecv(&anew[ind(0, 1)] /* east */, 1, east_west_type, west, 9, comm,
				&reqs[3]);

		MPI_Send(&anew[ind(1, 1)] /* north */, 1, north_south_type, north, 9,
				comm);
		MPI_Send(&anew[ind(1, by)] /* south */, 1, north_south_type, south, 9,
				comm);
		MPI_Send(&anew[ind(bx, 1)] /* east */, 1, east_west_type, east, 9,
				comm);
		MPI_Send(&anew[ind(1, 1)] /* west */, 1, east_west_type, west, 9, comm);

		MPI_Waitall(4, reqs, MPI_STATUSES_IGNORE);

		// swap arrays

		// swap with memcpy
		memcpy(aold, anew, (bx + 2) * (by + 2) * sizeof(double));

		// optional - print image
		if (iter == niters - 1)
			printarr_par(iter, anew, n, px, py, rx, ry, bx, by, offx, offy,
					comm);
	}
	t += MPI_Wtime();

	MPI_Type_free(&east_west_type);
	MPI_Type_free(&north_south_type);

	// get final heat in the system
	double rheat;
	MPI_Allreduce(&heat, &rheat, 1, MPI_DOUBLE, MPI_SUM, comm);
	if (!r)
		printf("[%i] last heat: %f time: %f\n", r, rheat, t);

	MPI_Finalize();
}
