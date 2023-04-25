# <b>LEOTP</b>
This is the code of paper "LEOTP: An Information-centric Transport Layer Protocol for LEO Satellite Networks" in ICDCS'23. LEOTP is based on a request-response model and in-network caching. The in-network retransmission provides a minimal cost of delay and bandwidth consumption for loss recovery. The backpressure-based hop-by-hop congestion control provides accurate traffic control, avoiding congesiton in the network while maximizing link utilization. Thus, LEOTP can achieve high throughput and low latency in LEO satellite networks. More details of LEOTP can be found in the paper.
### <b>Instructions</b>
1. Clone this repository:
````
git clone https://github.com/jl99888/LEOTP.git`
````
2. Compile the source code:
````
cd LEOTP/app
make
````
3. Run the application-layer program of LEOTP:

After step 2, application-layer programs of different role are generated in directory `app`, which are `appserver`, `appclient`, and `appmid`.
Supposing the topology of an entire link is $h1-gs1-m1-m2-\cdots-mk-gs2-h2$, where h1 and h2 are endpoints, gs1 and gs2 are ground stations, and m1~mk are LEO satellites. First, run `appmid` on all ground stations and satellites to enable segmented transport control. Then, run `appserver` on h2 to start server program. Last, run `appclient` to request data from the server. For simplification, we assume the ip addresses of the client and the sever are 10.0.1.1 and 10.0.100.2 respectively. When running on your own testbed or simulation environment, change those addresses correctly in `app/appclient.cpp` and `app/appserver.cpp` first.  



