#include "ZeroTierOne.h"
#include <string>

#include "Node.hpp"
#include "Phy.hpp"
#include "Binder.hpp"
#include "TestEthernetTap.hpp"
#include "ManagedRoute.hpp"
#include "Network.hpp"

using namespace ZeroTier;

// How often to check for local interface addresses
#define ZT_LOCAL_INTERFACE_CHECK_INTERVAL 60000

typedef TestEthernetTap EthernetTap;

class Service
{
public:

	/**
	 * Returned by node main if/when it terminates
	 */
	enum ReasonForTermination
	{
		/**
		 * Instance is still running
		 */
		ONE_STILL_RUNNING = 0,

		/**
		 * Normal shutdown
		 */
		ONE_NORMAL_TERMINATION = 1,

		/**
		 * A serious unrecoverable error has occurred
		 */
		ONE_UNRECOVERABLE_ERROR = 2,

		/**
		 * Your identity has collided with another
		 */
		ONE_IDENTITY_COLLISION = 3
	};

	/**
	 * Local settings for each network
	 */
	struct NetworkSettings
	{
		/**
		 * Allow this network to configure IP addresses and routes?
		 */
		bool allowManaged;

		/**
		 * Whitelist of addresses that can be configured by this network.
		 * If empty and allowManaged is true, allow all private/pseudoprivate addresses.
		 */
		std::vector<InetAddress> allowManagedWhitelist;

		/**
		 * Allow configuration of IPs and routes within global (Internet) IP space?
		 */
		bool allowGlobal;

		/**
		 * Allow overriding of system default routes for "full tunnel" operation?
		 */
		bool allowDefault;
	};

	// Configured networks
	struct NetworkState
	{
		NetworkState() :
			tap((EthernetTap *)0)
		{
			// Real defaults are in network 'up' code in network event handler
			settings.allowManaged = true;
			settings.allowGlobal = false;
			settings.allowDefault = false;
		}

		EthernetTap *tap;
		ZT_VirtualNetworkConfig config; // memcpy() of raw config from core
		std::vector<InetAddress> managedIps;
		std::list< SharedPtr<ManagedRoute> > managedRoutes;
		NetworkSettings settings;
	};
	std::map<uint64_t,NetworkState> _nets;
	Mutex _nets_m;
	
	// Termination status information
	ReasonForTermination _termReason;
	std::string _fatalErrorMessage;
	Mutex _termReason_m;

	// Deadline for the next background task service function
	volatile int64_t _nextBackgroundTaskDeadline;
	int64_t _lastRestart;

	std::vector< std::string > _interfacePrefixBlacklist;
	Mutex _localConfig_m;

	/*
	 * To attempt to handle NAT/gateway craziness we use three local UDP ports:
	 *
	 * [0] is the normal/default port, usually 9993
	 * [1] is a port dervied from our ZeroTier address
	 * [2] is a port computed from the normal/default for use with uPnP/NAT-PMP mappings
	 *
	 * [2] exists because on some gateways trying to do regular NAT-t interferes
	 * destructively with uPnP port mapping behavior in very weird buggy ways.
	 * It's only used if uPnP/NAT-PMP is enabled in this build.
	 */
	unsigned int _ports[3];
	unsigned int _primaryPort;
	Binder _binder;
	Phy<Service*> _phy;

	// ctor
	Service();

	// dtor
	~Service();

	Node *_node;
	void terminate();
	ReasonForTermination run();

	// Node event callbacks
	inline int nodeStateGetFunction(enum ZT_StateObjectType type,const uint64_t id[2],void *data,unsigned int maxlen);
	inline void nodeStatePutFunction(enum ZT_StateObjectType type,const uint64_t id[2],const void *data,int len);
	inline void nodeEventCallback(enum ZT_Event event,const void *metaData);
	inline int nodeWirePacketSendFunction(const int64_t localSocket,const struct sockaddr_storage *addr,const void *data,unsigned int len,unsigned int ttl);
	inline void nodeVirtualNetworkFrameFunction(uint64_t nwid,void **nuptr,uint64_t sourceMac,uint64_t destMac,unsigned int etherType,unsigned int vlanId,const void *data,unsigned int len);
	inline int nodeVirtualNetworkConfigFunction(uint64_t nwid,void **nuptr,enum ZT_VirtualNetworkConfigOperation op,const ZT_VirtualNetworkConfig *nwc);
	inline int nodePathCheckFunction(uint64_t ztaddr,const int64_t localSocket,const struct sockaddr_storage *remoteAddr);
	inline int nodePathLookupFunction(uint64_t ztaddr,int family,struct sockaddr_storage *result);

	// Phy callbacks
	inline void phyOnDatagram(PhySocket *sock,void **uptr,const struct sockaddr *localAddr,const struct sockaddr *from,void *data,unsigned long len);
	inline void phyOnTcpConnect(PhySocket *sock,void **uptr,bool success);
	inline void phyOnTcpAccept(PhySocket *sockL,PhySocket *sockN,void **uptrL,void **uptrN,const struct sockaddr *from);
	void phyOnTcpClose(PhySocket *sock,void **uptr);
	void phyOnTcpData(PhySocket *sock,void **uptr,void *data,unsigned long len);
	inline void phyOnTcpWritable(PhySocket *sock,void **uptr);
	inline void phyOnFileDescriptorActivity(PhySocket *sock,void **uptr,bool readable,bool writable);
	inline void phyOnUnixAccept(PhySocket *sockL,PhySocket *sockN,void **uptrL,void **uptrN);
	inline void phyOnUnixClose(PhySocket *sock,void **uptr);
	inline void phyOnUnixData(PhySocket *sock,void **uptr,void *data,unsigned long len);
	inline void phyOnUnixWritable(PhySocket *sock,void **uptr,bool lwip_invoked);

	// Misc
	bool _trialBind(unsigned int port);
	bool shouldBindInterface(const char *ifname,const InetAddress &ifaddr);
};