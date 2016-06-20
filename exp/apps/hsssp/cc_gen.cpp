/** ConnectedComp -*- C++ -*-
 * @file
 * @section License
 *
 * Galois, a framework to exploit amorphous data-parallelism in irregular
 * programs.
 *
 * Copyright (C) 2013, The University of Texas at Austin. All rights reserved.
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
 *
 * @section Description
 *
 * Compute ConnectedComp on distributed Galois.
 *
 * @author Gurbinder Gill <gurbinder533@gmail.com>
 */

#include <iostream>
#include <limits>
#include "Galois/Galois.h"
#include "Galois/gstl.h"
#include "Lonestar/BoilerPlate.h"
#include "Galois/Runtime/CompilerHelperFunctions.h"

#include "Galois/Runtime/OfflineGraph.h"
#include "Galois/Runtime/hGraph.h"
#include "Galois/DistAccumulator.h"
#include "Galois/Runtime/Tracer.h"


static const char* const name = "ConnectedComp - Distributed Heterogeneous";
static const char* const desc = "ConnectedComp on Distributed Galois.";
static const char* const url = 0;

namespace cll = llvm::cl;
static cll::opt<std::string> inputFile(cll::Positional, cll::desc("<input file>"), cll::Required);
static cll::opt<unsigned int> maxIterations("maxIterations", cll::desc("Maximum iterations: Default 1024"), cll::init(1024));
static cll::opt<unsigned int> src_node("srcNodeId", cll::desc("ID of the source node"), cll::init(0));
static cll::opt<bool> verify("verify", cll::desc("Verify ranks by printing to 'page_ranks.#hid.csv' file"), cll::init(false));


struct NodeData {
  std::atomic<unsigned long long> comp_current;
};

typedef hGraph<NodeData, void> Graph;
typedef typename Graph::GraphNode GNode;


struct InitializeGraph {
  Graph *graph;

  InitializeGraph(Graph* _graph) : graph(_graph){}
  void static go(Graph& _graph) {
    struct SyncerPull_0 {
      static unsigned long long extract(uint32_t node_id, const struct NodeData & node) {
#ifdef __GALOIS_HET_CUDA__
        if (personality == GPU_CUDA) return get_node_comp_current_cuda(cuda_ctx, node_id);
        assert (personality == CPU);
#endif
        return node.comp_current;
      }
      static void setVal (uint32_t node_id, struct NodeData & node, unsigned long long y) {
#ifdef __GALOIS_HET_CUDA__
        if (personality == GPU_CUDA) set_node_comp_current_cuda(cuda_ctx, node_id, y);
        else if (personality == CPU)
#endif
          node.comp_current = y;
      }
      typedef unsigned long long ValTy;
    };

#ifdef __GALOIS_HET_CUDA__
    if (personality == GPU_CUDA) {
      InitializeGraph_cuda(cuda_ctx);
    } else if (personality == CPU)
    #endif
    #ifdef __GALOIS_HET_CUDA__
    	if (personality == GPU_CUDA) {
    		InitializeGraph_cuda(cuda_ctx);
    	} else if (personality == CPU)
    #endif
    Galois::do_all(_graph.begin(), _graph.end(), InitializeGraph {&_graph}, Galois::loopname("InitGraph"));

    _graph.sync_pull<SyncerPull_0>();
  }

  void operator()(GNode src) const {
    NodeData& sdata = graph->getData(src);
    sdata.comp_current = graph->getGID(src);
  }
};

struct ConnectedComp {
  Graph* graph;
  static Galois::DGAccumulator<int> DGAccumulator_accum;

  ConnectedComp(Graph* _graph) : graph(_graph){}
  void static go(Graph& _graph){
    unsigned iteration = 0;
    do{
      DGAccumulator_accum.reset();

      #ifdef __GALOIS_HET_CUDA__
      	if (personality == GPU_CUDA) {
      		ConnectedComp_cuda(cuda_ctx);
      	} else if (personality == CPU)
      #endif
      	struct Syncer_0 {
      		static unsigned long long extract(uint32_t node_id, const struct NodeData & node) {
      		#ifdef __GALOIS_HET_CUDA__
      			if (personality == GPU_CUDA) return get_node_comp_current_cuda(cuda_ctx, node_id);
      			assert (personality == CPU);
      		#endif
      			return node.comp_current;
      		}
      		static void reduce (uint32_t node_id, struct NodeData & node, unsigned long long y) {
      		#ifdef __GALOIS_HET_CUDA__
      			if (personality == GPU_CUDA) add_node_comp_current_cuda(cuda_ctx, node_id, y);
      			else if (personality == CPU)
      		#endif
      				{ Galois::atomicMin(node.comp_current, y);}
      		}
      		static void reset (uint32_t node_id, struct NodeData & node ) {
      		#ifdef __GALOIS_HET_CUDA__
      			if (personality == GPU_CUDA) set_node_comp_current_cuda(cuda_ctx, node_id, 0);
      			else if (personality == CPU)
      		#endif
      				{node.comp_current = std::numeric_limits<unsigned long long>::max()/4; }
      		}
      		typedef unsigned long long ValTy;
      	};
      	struct SyncerPull_0 {
      		static unsigned long long extract(uint32_t node_id, const struct NodeData & node) {
      		#ifdef __GALOIS_HET_CUDA__
      			if (personality == GPU_CUDA) return get_node_comp_current_cuda(cuda_ctx, node_id);
      			assert (personality == CPU);
      		#endif
      			return node.comp_current;
      		}
      		static void setVal (uint32_t node_id, struct NodeData & node, unsigned long long y) {
      		#ifdef __GALOIS_HET_CUDA__
      			if (personality == GPU_CUDA) set_node_comp_current_cuda(cuda_ctx, node_id, y);
      			else if (personality == CPU)
      		#endif
      				node.comp_current = y;
      		}
      		typedef unsigned long long ValTy;
      	};
      #ifdef __GALOIS_HET_CUDA__
      	if (personality == GPU_CUDA) {
      		ConnectedComp_cuda(cuda_ctx);
      	} else if (personality == CPU)
      #endif
      Galois::do_all(_graph.begin(), _graph.end(), ConnectedComp { &_graph }, Galois::loopname("ConnectedComp"), Galois::write_set("sync_push", "this->graph", "struct NodeData &", "struct NodeData &" , "comp_current", "unsigned long long" , "{ Galois::atomicMin(node.comp_current, y);}",  "{node.comp_current = std::numeric_limits<unsigned long long>::max()/4; }"), Galois::write_set("sync_pull", "this->graph", "struct NodeData &", "struct NodeData &", "comp_current" , "unsigned long long"));
      _graph.sync_push<Syncer_0>();
      
      _graph.sync_pull<SyncerPull_0>();
      
     ++iteration;
     if(iteration >= maxIterations){
        DGAccumulator_accum.reset();
     }
    }while(DGAccumulator_accum.reduce());

    std::cout << " Total iteration run : " << iteration << "\n";
  }

  void operator()(GNode src) const {
    NodeData& snode = graph->getData(src);
    auto& sdist = snode.comp_current;

    for (auto jj = graph->edge_begin(src), ej = graph->edge_end(src); jj != ej; ++jj) {
      GNode dst = graph->getEdgeDst(jj);
      auto& dnode = graph->getData(dst);
      unsigned long long new_dist = sdist;
      auto old_dist = Galois::atomicMin(dnode.comp_current, new_dist);
      if(old_dist > new_dist){
        DGAccumulator_accum += 1;
      }
    }
  }
};
Galois::DGAccumulator<int>  ConnectedComp::DGAccumulator_accum;

/********Set source Node ************/
void setSource(Graph& _graph){
  auto& net = Galois::Runtime::getSystemNetworkInterface();
  if(net.ID == 0){
    auto& nd = _graph.getData(src_node);
    nd.comp_current = 0;
  }
}

int main(int argc, char** argv) {
  try {
    LonestarStart(argc, argv, name, desc, url);
    auto& net = Galois::Runtime::getSystemNetworkInterface();
    Galois::Timer T_total, T_offlineGraph_init, T_hGraph_init, T_init, T_ConnectedComp1, T_ConnectedComp2, T_ConnectedComp3;

    T_total.start();

    T_hGraph_init.start();
    Graph hg(inputFile, net.ID, net.Num);
    T_hGraph_init.stop();

    std::cout << "InitializeGraph::go called\n";
    T_init.start();
    InitializeGraph::go(hg);
    T_init.stop();

    // Verify

#if 0
    if(verify){
        for(auto ii = hg.begin(); ii != hg.end(); ++ii) {
          std::cout << "[" << *ii << "]  " << hg.getData(*ii).comp_current << "\n";
        }
    }
#endif


    std::cout << "ConnectedComp::go run1 called  on " << net.ID << "\n";
    T_ConnectedComp1.start();
      ConnectedComp::go(hg);
    T_ConnectedComp1.stop();

    std::cout << "[" << net.ID << "]" << " Total Time : " << T_total.get() << " offlineGraph : " << T_offlineGraph_init.get() << " hGraph : " << T_hGraph_init.get() << " Init : " << T_init.get() << " ConnectedComp1 : " << T_ConnectedComp1.get() << " (msec)\n\n";

    Galois::Runtime::getHostBarrier().wait();
    InitializeGraph::go(hg);

    std::cout << "ConnectedComp::go run2 called  on " << net.ID << "\n";
    T_ConnectedComp2.start();
      ConnectedComp::go(hg);
    T_ConnectedComp2.stop();

    std::cout << "[" << net.ID << "]" << " Total Time : " << T_total.get() << " offlineGraph : " << T_offlineGraph_init.get() << " hGraph : " << T_hGraph_init.get() << " Init : " << T_init.get() << " ConnectedComp2 : " << T_ConnectedComp2.get() << " (msec)\n\n";

    Galois::Runtime::getHostBarrier().wait();
    InitializeGraph::go(hg);

    std::cout << "ConnectedComp::go run3 called  on " << net.ID << "\n";
    T_ConnectedComp3.start();
      ConnectedComp::go(hg);
    T_ConnectedComp3.stop();

    std::cout << "[" << net.ID << "]" << " Total Time : " << T_total.get() << " offlineGraph : " << T_offlineGraph_init.get() << " hGraph : " << T_hGraph_init.get() << " Init : " << T_init.get() << " ConnectedComp3 : " << T_ConnectedComp3.get() << " (msec)\n\n";


   T_total.stop();

    auto mean_time = (T_ConnectedComp1.get() + T_ConnectedComp2.get() + T_ConnectedComp3.get())/3;

    std::cout << "[" << net.ID << "]" << " Total Time : " << T_total.get() << " offlineGraph : " << T_offlineGraph_init.get() << " hGraph : " << T_hGraph_init.get() << " Init : " << T_init.get() << " ConnectedComp1 : " << T_ConnectedComp1.get() << " ConnectedComp2 : " << T_ConnectedComp2.get() << " ConnectedComp3 : " << T_ConnectedComp3.get() <<" ConnectedComp mean time (3 runs ) (" << maxIterations << ") : " << mean_time << "(msec)\n\n";

    if(verify){
      for(auto ii = hg.begin(); ii != hg.end(); ++ii) {
        Galois::Runtime::printOutput("% %\n", hg.getGID(*ii), hg.getData(*ii).comp_current);
      }
    }
    return 0;
  } catch(const char* c) {
    std::cerr << "Error: " << c << "\n";
      return 1;
  }
}
