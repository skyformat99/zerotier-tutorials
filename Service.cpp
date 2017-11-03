#include "Service.h"
#include "libztDebug.h"
#include "BasicUtils.h"

#include "Packet.hpp"

#include <stdint.h>
#include <stdio.h>
#include <arpa/inet.h>

static int SnodeVirtualNetworkConfigFunction(ZT_Node *node,void *uptr,void *tptr,uint64_t nwid,void **nuptr,enum ZT_VirtualNetworkConfigOperation op,const ZT_VirtualNetworkConfig *nwconf)
{ return reinterpret_cast<Service *>(uptr)->nodeVirtualNetworkConfigFunction(nwid,nuptr,op,nwconf); }
static void SnodeEventCallback(ZT_Node *node,void *uptr,void *tptr,enum ZT_Event event,const void *metaData)
{ reinterpret_cast<Service *>(uptr)->nodeEventCallback(event,metaData); }
static void SnodeStatePutFunction(ZT_Node *node,void *uptr,void *tptr,enum ZT_StateObjectType type,const uint64_t id[2],const void *data,int len)
{ reinterpret_cast<Service *>(uptr)->nodeStatePutFunction(type,id,data,len); }
static int SnodeStateGetFunction(ZT_Node *node,void *uptr,void *tptr,enum ZT_StateObjectType type,const uint64_t id[2],void *data,unsigned int maxlen)
{ return reinterpret_cast<Service *>(uptr)->nodeStateGetFunction(type,id,data,maxlen); }
static int SnodeWirePacketSendFunction(ZT_Node *node,void *uptr,void *tptr,int64_t localSocket,const struct sockaddr_storage *addr,const void *data,unsigned int len,unsigned int ttl)
{ return reinterpret_cast<Service *>(uptr)->nodeWirePacketSendFunction(localSocket,addr,data,len,ttl); }
static void SnodeVirtualNetworkFrameFunction(ZT_Node *node,void *uptr,void *tptr,uint64_t nwid,void **nuptr,uint64_t sourceMac,uint64_t destMac,unsigned int etherType,unsigned int vlanId,const void *data,unsigned int len)
{ reinterpret_cast<Service *>(uptr)->nodeVirtualNetworkFrameFunction(nwid,nuptr,sourceMac,destMac,etherType,vlanId,data,len); }
static int SnodePathCheckFunction(ZT_Node *node,void *uptr,void *tptr,uint64_t ztaddr,int64_t localSocket,const struct sockaddr_storage *remoteAddr)
{ return reinterpret_cast<Service *>(uptr)->nodePathCheckFunction(ztaddr,localSocket,remoteAddr); }
static int SnodePathLookupFunction(ZT_Node *node,void *uptr,void *tptr,uint64_t ztaddr,int family,struct sockaddr_storage *result)
{ return reinterpret_cast<Service *>(uptr)->nodePathLookupFunction(ztaddr,family,result); }
//static void StapFrameHandler(void *uptr,void *tptr,uint64_t nwid,const MAC &from,const MAC &to,unsigned int etherType,unsigned int vlanId,const void *data,unsigned int len)
//{ reinterpret_cast<Service *>(uptr)->tapFrameHandler(nwid,from,to,etherType,vlanId,data,len); }

// ctor
Service::Service():
_phy(this, false, true)
{
	DEBUG_INFO();
	_primaryPort = 9999;
	_ports[0] = 0;
	_ports[1] = 0;
	_ports[2] = 0;
}

// dtor
Service::~Service()
{
	DEBUG_INFO();
}

Service::ReasonForTermination Service::run() 
{
	DEBUG_INFO();
	struct ZT_Node_Callbacks cb;
	ReasonForTermination _termReason;

	try
	{
		{
			// set callback functions
			cb.version = 0;
			cb.stateGetFunction = SnodeStateGetFunction;
			cb.statePutFunction = SnodeStatePutFunction;
			cb.eventCallback = SnodeEventCallback;
			cb.wirePacketSendFunction = SnodeWirePacketSendFunction;
			cb.virtualNetworkFrameFunction = SnodeVirtualNetworkFrameFunction;
			cb.virtualNetworkConfigFunction = SnodeVirtualNetworkConfigFunction;
			cb.pathCheckFunction = SnodePathCheckFunction;
			cb.pathLookupFunction = SnodePathLookupFunction;
			_node = new Node(this, (void*)0, &cb, now());
		}
	} catch ( ... ) {
		//Mutex::Lock _l(_termReason_m);
		_termReason = ONE_UNRECOVERABLE_ERROR;
		_fatalErrorMessage = "unexpected exception in main thread";
		DEBUG_ERROR("unexpected exception in main thread");
		return _termReason;
	}

	// Make sure we can use the primary port, and hunt for one if configured to do so
	const int portTrials = (_primaryPort == 0) ? 256 : 1; // if port is 0, pick random
	for(int k=0;k<portTrials;++k) {
		if (_primaryPort == 0) {
			unsigned int randp = 0;
			Utils::getSecureRandom(&randp,sizeof(randp));
			_primaryPort = 20000 + (randp % 45500);
		}
		DEBUG_INFO("trying port=%d", _primaryPort);
		if (_trialBind(_primaryPort)) {
			_ports[0] = _primaryPort;
		} else {
			_primaryPort = 0;
		}
	}
	if (_ports[0] == 0) {
		Mutex::Lock _l(_termReason_m);
		_termReason = ONE_UNRECOVERABLE_ERROR;
		_fatalErrorMessage = "cannot bind to local control interface port";
		DEBUG_ERROR("cannot bind to local control interface port");
		return _termReason;
	}

	// Attempt to bind to a secondary port chosen from our ZeroTier address.
	// This exists because there are buggy NATs out there that fail if more
	// than one device behind the same NAT tries to use the same internal
	// private address port number. Buggy NATs are a running theme.
	_ports[1] = 20000 + ((unsigned int)_node->address() % 45500);
	for(int i=0;;++i) {
		if (i > 1000) {
			_ports[1] = 0;
			break;
		} else if (++_ports[1] >= 65536) {
			_ports[1] = 20000;
		}
		if (_trialBind(_ports[1]))
			break;
	}

	// Main I/O Loop
	_nextBackgroundTaskDeadline = 0;
	int64_t clockShouldBe = now();
	_lastRestart = clockShouldBe;
	int64_t lastTapMulticastGroupCheck = 0;
	int64_t lastBindRefresh = 0;
	int64_t lastUpdateCheck = clockShouldBe;
	int64_t lastCleanedPeersDb = 0;
	int64_t lastLocalInterfaceAddressCheck = (clockShouldBe - ZT_LOCAL_INTERFACE_CHECK_INTERVAL) + 15000; // do this in 15s to give portmapper time to configure and other things time to settle
	
	for(;;)
	{
		const int64_t now_ts = now();

		// Attempt to detect sleep/wake events by detecting delay overruns
		bool restarted = false;
		if ((now_ts > clockShouldBe)&&((now_ts - clockShouldBe) > 10000)) {
			_lastRestart = now_ts;
			restarted = true;
		}

		// Refresh bindings in case device's interfaces have changed, and also sync routes to update any shadow routes (e.g. shadow default)
		if (((now_ts - lastBindRefresh) >= ZT_BINDER_REFRESH_PERIOD)||(restarted)) {
			lastBindRefresh = now_ts;
			unsigned int p[3];
			unsigned int pc = 0;
			for(int i=0;i<3;++i) {
				if (_ports[i])
					p[pc++] = _ports[i];
			}
			_binder.refresh(_phy,p,pc,*this);
			{
			}
		}

		// Run background task processor in core if it's time to do so
		int64_t dl = _nextBackgroundTaskDeadline;
		if (dl <= now_ts) {
			_node->processBackgroundTasks((void *)0,now_ts,&_nextBackgroundTaskDeadline);
			dl = _nextBackgroundTaskDeadline;
		}
		const unsigned long delay = (dl > now_ts) ? (unsigned long)(dl - now_ts) : 100;
		clockShouldBe = now_ts + (uint64_t)delay;
		_phy.poll(delay);
	}
	return _termReason;
}

// shut down node, service
void Service::terminate()
{
	DEBUG_INFO();
}

// --- Node event callbacks --- 


inline int Service::nodeStateGetFunction(enum ZT_StateObjectType type,const uint64_t id[2],void *data,unsigned int maxlen)
{
	DEBUG_INFO();
	return 0;
}

inline void Service::nodeStatePutFunction(enum ZT_StateObjectType type,const uint64_t id[2],const void *data,int len)
{
	DEBUG_INFO("type=%d, buf=%s", type, data);
	return;
}

inline void Service::nodeEventCallback(enum ZT_Event event,const void *metaData)
{
	switch(event)
	{
		case ZT_EVENT_UP: {
			DEBUG_INFO("ZT_EVENT_UP");
		} break;
		case ZT_EVENT_FATAL_ERROR_IDENTITY_COLLISION: {
			DEBUG_INFO("identity collision");
		} break;
		case ZT_EVENT_OFFLINE: {
			DEBUG_INFO("ZT_EVENT_OFFLINE");
		} break;
		case ZT_EVENT_ONLINE: {
			DEBUG_INFO("ZT_EVENT_ONLINE");
		} break;
		case ZT_EVENT_DOWN: {
			DEBUG_INFO("ZT_EVENT_DOWN");
		} break;
		default: {
			break;
		}
	}
	return;
}

inline int Service::nodeWirePacketSendFunction(const int64_t localSocket,const struct sockaddr_storage *addr,const void *data,unsigned int len,unsigned int ttl)
{
	Packet p(data, len);
	char ipdest[128];
	p.destination().toString(ipdest);
	//struct sockaddr_in *sin = (struct sockaddr_in *)addr;
	//DEBUG_INFO("%s : %d", inet_ntoa(sin->sin_addr), ntohs(sin->sin_port));

#ifdef ZT_TCP_FALLBACK_RELAY
	DEBUG_INFO("ZT_TCP_FALLBACK_RELAY, verb=%x, dest=%s, localSocket=%d, len=%d, ttl=%d", p.verb(), ipdest, localSocket, len, ttl);
#else // ZT_TCP_FALLBACK_RELAY
	DEBUG_INFO("UDP, verb=%x, dest=%s, localSocket=%d, len=%d, ttl=%d", p.verb(), ipdest, localSocket, len, ttl);
	if ((localSocket != -1)&&(localSocket != 0)&&(_binder.isUdpSocketValid((PhySocket *)((uintptr_t)localSocket)))) {
		if ((ttl)&&(addr->ss_family == AF_INET)) _phy.setIp4UdpTtl((PhySocket *)((uintptr_t)localSocket),ttl);
		const bool r = _phy.udpSend((PhySocket *)((uintptr_t)localSocket),(const struct sockaddr *)addr,data,len);
		if ((ttl)&&(addr->ss_family == AF_INET)) _phy.setIp4UdpTtl((PhySocket *)((uintptr_t)localSocket),255);
		return ((r) ? 0 : -1);
	} else {
		int r = (_binder.udpSendAll(_phy,addr,data,len,ttl));
		return (r ? 0 : -1);
	}
#endif
}



inline void Service::nodeVirtualNetworkFrameFunction(uint64_t nwid,void **nuptr,uint64_t sourceMac,uint64_t destMac,unsigned int etherType,unsigned int vlanId,const void *data,unsigned int len)
{
	DEBUG_INFO();
	NetworkState *n = reinterpret_cast<NetworkState *>(*nuptr);
	if ((!n)||(!n->tap))
		return;
	n->tap->put(MAC(sourceMac),MAC(destMac),etherType,data,len);
}

inline int Service::nodeVirtualNetworkConfigFunction(uint64_t nwid,void **nuptr,enum ZT_VirtualNetworkConfigOperation op,const ZT_VirtualNetworkConfig *nwc)
{
	DEBUG_INFO();
}
inline int Service::nodePathCheckFunction(uint64_t ztaddr,const int64_t localSocket,const struct sockaddr_storage *remoteAddr)
{
	//DEBUG_INFO();
	return 1;
}
inline int Service::nodePathLookupFunction(uint64_t ztaddr,int family,struct sockaddr_storage *result)
{
	DEBUG_INFO();
}

// --- Phy callbacks --- 


 
inline void Service::phyOnDatagram(PhySocket *sock,void **uptr,const struct sockaddr *localAddr,const struct sockaddr *from,void *data,unsigned long len)
{
	//DEBUG_INFO("len=%d", len);
	const ZT_ResultCode rc = _node->processWirePacket(
			(void *)0,
			now(),
			reinterpret_cast<int64_t>(sock),
			reinterpret_cast<const struct sockaddr_storage *>(from), // Phy<> uses sockaddr_storage, so it'll always be that big
			data,
			len,
			&_nextBackgroundTaskDeadline);
}
inline void Service::phyOnTcpConnect(PhySocket *sock,void **uptr,bool success)
{
	DEBUG_INFO();
}
inline void Service::phyOnTcpAccept(PhySocket *sockL,PhySocket *sockN,void **uptrL,void **uptrN,const struct sockaddr *from)
{
	DEBUG_INFO();
}
void Service::phyOnTcpClose(PhySocket *sock,void **uptr)
{
	DEBUG_INFO();
}
void Service::phyOnTcpData(PhySocket *sock,void **uptr,void *data,unsigned long len)
{
	DEBUG_INFO();
}
inline void Service::phyOnTcpWritable(PhySocket *sock,void **uptr)
{
	DEBUG_INFO();
}
inline void Service::phyOnFileDescriptorActivity(PhySocket *sock,void **uptr,bool readable,bool writable) 
{
	DEBUG_INFO();
}
inline void Service::phyOnUnixAccept(PhySocket *sockL,PhySocket *sockN,void **uptrL,void **uptrN) 
{
	DEBUG_INFO();
}
inline void Service::phyOnUnixClose(PhySocket *sock,void **uptr) 
{
	DEBUG_INFO();
}
inline void Service::phyOnUnixData(PhySocket *sock,void **uptr,void *data,unsigned long len) 
{
	DEBUG_INFO();
}
inline void Service::phyOnUnixWritable(PhySocket *sock,void **uptr,bool lwip_invoked) 
{
	DEBUG_INFO();
}


// --- Misc ---

bool Service::_trialBind(unsigned int port)
{
	struct sockaddr_in in4;
	struct sockaddr_in6 in6;
	PhySocket *tb;

	memset(&in4,0,sizeof(in4));
	in4.sin_family = AF_INET;
	in4.sin_port = Utils::hton((uint16_t)port);
	tb = _phy.udpBind(reinterpret_cast<const struct sockaddr *>(&in4),(void *)0,0);
	if (tb) {
		_phy.close(tb,false);
		tb = _phy.tcpListen(reinterpret_cast<const struct sockaddr *>(&in4),(void *)0);
		if (tb) {
			_phy.close(tb,false);
			return true;
		}
	}

	memset(&in6,0,sizeof(in6));
	in6.sin6_family = AF_INET6;
	in6.sin6_port = Utils::hton((uint16_t)port);
	tb = _phy.udpBind(reinterpret_cast<const struct sockaddr *>(&in6),(void *)0,0);
	if (tb) {
		_phy.close(tb,false);
		tb = _phy.tcpListen(reinterpret_cast<const struct sockaddr *>(&in6),(void *)0);
		if (tb) {
			_phy.close(tb,false);
			return true;
		}
	}

	return false;
}


	bool Service::shouldBindInterface(const char *ifname,const InetAddress &ifaddr)
	{
#if defined(__linux__) || defined(linux) || defined(__LINUX__) || defined(__linux)
		if ((ifname[0] == 'l')&&(ifname[1] == 'o')) return false; // loopback
		if ((ifname[0] == 'z')&&(ifname[1] == 't')) return false; // sanity check: zt#
		if ((ifname[0] == 't')&&(ifname[1] == 'u')&&(ifname[2] == 'n')) return false; // tun# is probably an OpenVPN tunnel or similar
		if ((ifname[0] == 't')&&(ifname[1] == 'a')&&(ifname[2] == 'p')) return false; // tap# is probably an OpenVPN tunnel or similar
#endif

#ifdef __APPLE__
		if ((ifname[0] == 'l')&&(ifname[1] == 'o')) return false; // loopback
		if ((ifname[0] == 'z')&&(ifname[1] == 't')) return false; // sanity check: zt#
		if ((ifname[0] == 't')&&(ifname[1] == 'u')&&(ifname[2] == 'n')) return false; // tun# is probably an OpenVPN tunnel or similar
		if ((ifname[0] == 't')&&(ifname[1] == 'a')&&(ifname[2] == 'p')) return false; // tap# is probably an OpenVPN tunnel or similar
		if ((ifname[0] == 'u')&&(ifname[1] == 't')&&(ifname[2] == 'u')&&(ifname[3] == 'n')) return false; // ... as is utun#
#endif

		{
			Mutex::Lock _l(_localConfig_m);
			for(std::vector<std::string>::const_iterator p(_interfacePrefixBlacklist.begin());p!=_interfacePrefixBlacklist.end();++p) {
				if (!strncmp(p->c_str(),ifname,p->length()))
					return false;
			}
		}

		{
			Mutex::Lock _l(_nets_m);
			for(std::map<uint64_t,NetworkState>::const_iterator n(_nets.begin());n!=_nets.end();++n) {
				if (n->second.tap) {
					std::vector<InetAddress> ips(n->second.tap->ips());
					for(std::vector<InetAddress>::const_iterator i(ips.begin());i!=ips.end();++i) {
						if (i->ipsEqual(ifaddr))
							return false;
					}
				}
			}
		}

		return true;
	}

