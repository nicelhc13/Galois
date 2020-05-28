Breadth First Search
================================================================================

DESCRIPTION 
--------------------------------------------------------------------------------

This program performs breadth-first search (bfs) on an input graph, starting from a
source node (specified by -startNode option). 

The algorithm is a bulk-synchronous parallel version. Lonestar has push- and pull-
based bfs.

First, in the bfs-push, a node that has been updated from the last round will push
out its distance value to its neighbors and update them if necessary in each round.
Second, in the bfs-pull, every node will check its neighbors' distance values and update
their own values based on what they see in each round.

INPUT
--------------------------------------------------------------------------------

Takes in Galois .gr graphs.

BUILD
--------------------------------------------------------------------------------

1. Run cmake at BUILD directory (refer to top-level README for cmake instructions).

2. Run `cd <BUILD>/dist-apps/; make -j

RUN
--------------------------------------------------------------------------------

To run on 1 host with start node 0, use the following:
`./bfs_push <input-graph> -graphTranspose=<transpose-input-graph> -t=<num-threads>`
`./bfs_pull <input-graph> -graphTranspose=<transpose-input-graph> -t=<num-threads>`

To run on 3 hosts h1, h2, and h3 for start node 0, use the following:
`mpirun -n=3 -hosts=h1,h2,h3 ./bfs_push <input-graph> -graphTranspose=<transpose-input-graph> -t=<num-threads>`
`mpirun -n=3 -hosts=h1,h2,h3 ./bfs_pull <input-graph> -graphTranspose=<transpose-input-graph> -t=<num-threads>`

To run on 3 hosts h1, h2, and h3 for start node 10 with an incoming edge cut, use the following:
`mpirun -n=3 -hosts=h1,h2,h3 ./bfs_push <input-graph> -graphTranspose=<transpose-input-graph> -t=<num-threads> -startNode=10 -partition=iec`
`mpirun -n=3 -hosts=h1,h2,h3 ./bfs_pull <input-graph> -graphTranspose=<transpose-input-graph> -t=<num-threads> -startNode=10 -partition=iec`

PERFORMANCE
--------------------------------------------------------------------------------

Note that atomics are used in the push version, meaning if there is high
contention for a node, performance may suffer.

The pull style version of distributed BFS generally does not perform as well as 
the push style version from our experience.

Additionally, load balancing among hosts may be an important factor to consider
when partitioning the graph.
