# <b>LEOTP</b>
This is the code of paper "LEOTP: An Information-centric Transport Layer Protocol for LEO Satellite Networks" in ICDCS'23.
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

After step 2, application-layer programs of different role are generated, which are `appserver`, `appclient`, and `appmid`.
We assume the topology of an entire link is $h1-gs1-m1-m2-\cdots-mk-gs2-h2$. h1 and h2 are endpoints, gs1 and gs2 are ground stations, and m1~mk are LEO satellites. First, we run `appmid` on all ground stations and satellites to enable segmented transport control. Then, we run `appserver` on h2 to start server program. Last, we run `appclient` to request data from the server.



