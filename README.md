# <b>LEOTP</b>
This is the code in paper "LEOTP: An Information-centric Transport Layer Protocol for LEO Satellite Networks" in ICDCS'23. LEOTP is based on a request-response model and in-network caching. The in-network retransmission provides a minimal cost of delay and bandwidth consumption for loss recovery. The backpressure-based hop-by-hop congestion control provides accurate traffic control, avoiding congesiton in the network while maximizing link utilization. Thus, LEOTP achieves high throughput and low latency in LEO satellite networks. More details of LEOTP can be found in the paper.
### <b>Requirements</b>
The code nees to be compiled and run under LINUX OS (e.g., Ubuntu 20.04).
### <b>Instructions</b>
1. Clone this repository:
````
git clone https://github.com/jl99888/LEOTP.git
````
2. Compile the source code:
````
cd LEOTP/app
make
````
3. Run application-layer programs of LEOTP:

After step 2, three application-layer programs of different roles are generated in directory `app`, which are `appserver`, `appclient`, and `appmid`. To run them, use commands:
````
./appserver -s serverip
./appmid
./appclient -c clientip -s serverip
````
Supposing the topology of an entire link is $h1-gs1-m1-m2-\cdots-mk-gs2-h2$, where h1 and h2 are endpoints, gs1 and gs2 are ground stations, and m1~mk are LEO satellites. First, run `appmid` on all ground stations and satellites to enable segmented transmission control. Then, run `appserver` on h2 to start server program. At last, run `appclient` to request data from the server. we assume the default ip addresses of the client and the sever are 10.0.1.1 and 10.0.100.2 respectively. When running on your own testbed or simulation environment, use `-c` and `-s` options to configure those addresses correctly.

4. Further usage:




