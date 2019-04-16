/*
 * This file belongs to the Galois project, a C++ library for exploiting parallelism.
 * The code is being released under the terms of the 3-Clause BSD License (a
 * copy is located in LICENSE.txt at the top-level directory).
 *
 * Copyright (C) 2018, The University of Texas at Austin. All rights reserved.
 * UNIVERSITY EXPRESSLY DISCLAIMS ANY AND ALL WARRANTIES CONCERNING THIS
 * SOFTWARE AND DOCUMENTATION, INCLUDING ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR ANY PARTICULAR PURPOSE, NON-INFRINGEMENT AND WARRANTIES OF
 * PERFORMANCE, AND ANY WARRANTY THAT MIGHT OTHERWISE ARISE FROM COURSE OF
 * DEALING OR USAGE OF TRADE.  NO WARRANTY IS EITHER EXPRESS OR IMPLIED WITH
 * RESPECT TO THE USE OF THE SOFTWARE OR DOCUMENTATION. Under no circumstances
 * shall University be liable for incidental, special, indirect, direct or
 * consequential damages or loss of profits, interruption of business, or
 * related expenses which may arise from use of Software or Documentation,
 * including but not limited to those resulting from defects in Software and/or
 * Documentation, or loss or inaccuracy of data of any kind.
 */

#include "galois/Galois.h"
#include "galois/Reduction.h"
#include "galois/Bag.h"
#include "galois/Timer.h"
#include "galois/graphs/LCGraph.h"
#include "galois/ParallelSTL.h"
#include "llvm/Support/CommandLine.h"
#include "Lonestar/BoilerPlate.h"
#include "galois/runtime/Profile.h"
#include <boost/iterator/transform_iterator.hpp>
#include <utility>
#include <vector>
#include <algorithm>
#include <iostream>
#include <fstream>
#define USE_SIMPLE
#define DEBUG 0
#define ENABLE_LABEL 0

const char* name = "TC";
const char* desc = "Counts the triangles in a graph ((only works for undirected neighbor-sorted graphs)";
const char* url  = 0;

enum Algo {
	nodeiterator,
	edgeiterator,
};

namespace cll = llvm::cl;
static cll::opt<std::string> filetype(cll::Positional, cll::desc("<filetype>"), cll::Required);
static cll::opt<std::string> filename(cll::Positional, cll::desc("<filename>"), cll::Required);
static cll::opt<Algo> algo("algo", cll::desc("Choose an algorithm:"), cll::values(
	clEnumValN(Algo::nodeiterator, "nodeiterator", "Node Iterator"),
	clEnumValN(Algo::edgeiterator, "edgeiterator", "Edge Iterator"), clEnumValEnd),
	cll::init(Algo::nodeiterator));
typedef galois::graphs::LC_CSR_Graph<uint32_t, void>::with_numa_alloc<true>::type ::with_no_lockable<true>::type Graph;
typedef Graph::GraphNode GNode;

#include "Lonestar/mgraph.h"
#include "Mining/util.h"

void TcSolver(Graph& graph) {
	galois::GAccumulator<unsigned int> total_num;
	total_num.reset();
	//galois::do_all(
		//galois::iterate(graph),
		//[&](const GNode& src) {
	galois::for_each(
		galois::iterate(graph.begin(), graph.end()),
		[&](const GNode& src, auto& ctx) {
			for (auto e1 : graph.edges(src)) {
				GNode dst = graph.getEdgeDst(e1);
				if (dst > src) break;
				for (auto e2 : graph.edges(dst)) {
					GNode dst_dst = graph.getEdgeDst(e2);
					if (dst_dst > dst) break;
					for (auto e3 : graph.edges(src)) {
						GNode dst2 = graph.getEdgeDst(e3);
						if (dst_dst == dst2) {
							total_num += 1;
							break;
						}
						if (dst2 > dst_dst) break;
					}
				}
			}
		},
		galois::chunk_size<512>(), galois::steal(), 
		galois::loopname("Couting")
	);
	galois::gPrint("total_num_triangles = ", total_num.reduce(), "\n\n");
}

int main(int argc, char** argv) {
	galois::SharedMemSys G;
	LonestarStart(argc, argv, name, desc, url);
	Graph graph;
	MGraph mgraph;
	galois::StatTimer Tinitial("GraphReadingTime");
	Tinitial.start();
	if (filetype == "txt") {
		printf("Reading .lg file: %s\n", filename.c_str());
		mgraph.read_txt(filename.c_str());
		genGraph(mgraph, graph);
	} else if (filetype == "adj") {
		printf("Reading .adj file: %s\n", filename.c_str());
		mgraph.read_adj(filename.c_str());
		genGraph(mgraph, graph);
	} else if (filetype == "mtx") {
		printf("Reading .mtx file: %s\n", filename.c_str());
		mgraph.read_mtx(filename.c_str(), true); //symmetrize
		genGraph(mgraph, graph);
	} else if (filetype == "gr") {
		printf("Reading .gr file: %s\n", filename.c_str());
		galois::graphs::readGraph(graph, filename);
		for (GNode n : graph) graph.getData(n) = 1;
	} else { printf("Unkown file format\n"); exit(1); }
	Tinitial.stop();
	galois::gPrint("num_vertices ", graph.size(), " num_edges ", graph.sizeEdges(), "\n\n");
	galois::reportPageAlloc("MeminfoPre");
	galois::StatTimer T;
	T.start();
	switch (algo) {
		case nodeiterator:
			TcSolver(graph);
			break;
		case edgeiterator:
			break;
		default:
			std::cerr << "Unknown algo: " << algo << "\n";
	}
	T.stop();
	galois::reportPageAlloc("MeminfoPost");
	return 0;
}
