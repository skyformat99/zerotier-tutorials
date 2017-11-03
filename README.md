## ZeroTier Core Walkthrough
***

In this document we are going to cover the steps necessary to build an application which utilizes the ZeroTier network virtualization engine. This type of integration is aimed at high performance or specialty applications which must control every aspect of the communication process. If you'd instead be satisfied with a less complex POSIX-like and easier to use socket API, see: [libzt](https://github.com/zerotier/libzt).

For this type of integration, you'll need to create your own timing loop, specify callbacks for the virtual network functions, implement local file data storage and retreival routines for your platform and handle periodic Node state changes. You should take a look at the example [Service](Service.cpp) code for I will only be including the most relevant snippets in this document. There is more support code provided in the source files than I will show here.

### Building the core

Clone the [ZeroTierOne](https://github.com/zerotier/ZeroTierOne) repo. `make core`. link the resultant `libzerotiercore.a` with this application.

### Bringing your node online

The Node is your main concern. You will need to instantiate one and create an I/O timing loop to manage it's state. For our example we've created a handler class called `Service`. Within `Service` we will implement `run()` which will be responsible for creating the `Node` object, registering its callbacks, and calling `Node.processBackgroundTasks()` periodically:

#### Register and implement the Node's callbacks:

```
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
```

For basic operation you'll only need to implement `SnodeEventCallback` to monitor and act upon state changes in the node, and `SnodeWirePacketSendFunction` for sending the packets out onto the physical network. `SnodePathCheckFunction` can simply return `true` for now. And we can ignore `SnodeStateGetFunction` and `SnodeStatePutFunction` which we'll later use to store the node's public and private identity keys.

#### Bind to ports for underlying UDP communication with ZeroTier root servers:

```
Not shown here, see source
```

#### Handling Node events and state changes

Information about the various node events can be found in [ZeroTierOne.h](zto/include/ZeroTierOne.h)

```
inline void Service::nodeEventCallback(enum ZT_Event event,const void *metaData)
{
	switch(event)
	{
		case ZT_EVENT_UP: {
		} break;
		case ZT_EVENT_FATAL_ERROR_IDENTITY_COLLISION: {
		} break;
		case ZT_EVENT_OFFLINE: {
		} break;
		case ZT_EVENT_ONLINE: {
		} break;
		case ZT_EVENT_DOWN: {
		} break;
		default: {
			break;
		}
	}
	return;
}
```

#### Saying HELLO

For detailed information on the ZeroTier protocol verbs, see [Packet.hpp]()

```
inline int Service::nodeWirePacketSendFunction(const int64_t localSocket,const struct sockaddr_storage *addr,const void *data,unsigned int len,unsigned int ttl)
{
#ifdef ZT_TCP_FALLBACK_RELAY
	// TCP fallback code
#else // ZT_TCP_FALLBACK_RELAY
	if ((localSocket != -1)&&(localSocket != 0)&&(_binder.isUdpSocketValid((PhySocket *)((uintptr_t)localSocket)))) {
		if ((ttl)&&(addr->ss_family == AF_INET)) _phy.setIp4UdpTtl((PhySocket *)((uintptr_t)localSocket),ttl);
		const bool r = _phy.udpSend((PhySocket *)((uintptr_t)localSocket),(const struct sockaddr *)addr,data,len);
		if ((ttl)&&(addr->ss_family == AF_INET)) _phy.setIp4UdpTtl((PhySocket *)((uintptr_t)localSocket),255);
		return ((r) ? 0 : -1);
	} else {
		return ((_binder.udpSendAll(_phy,addr,data,len,ttl)) ? 0 : -1);
	}
#endif
}
```

#### Feeding received packets into the Node

```
inline void Service::phyOnDatagram(PhySocket *sock,void **uptr,const struct sockaddr *localAddr,const struct sockaddr *from,void *data,unsigned long len)
{
	const ZT_ResultCode rc = _node->processWirePacket(
			(void *)0,
			now(),
			reinterpret_cast<int64_t>(sock),
			reinterpret_cast<const struct sockaddr_storage *>(from), // Phy<> uses sockaddr_storage, so it'll always be that big
			data,
			len,
			&_nextBackgroundTaskDeadline);
}
```

#### Storing (and retreiving) your Node's configuration 

We won't show any code here since it's platform dependent but the general idea is to implement `SnodeStateGetFunction()` and `SnodeStatePutFunction()`. These functions will handle identity keys, planet configurations, federated root servers, network configs, and information about known peers. 

#### Sending a VIRTUAL frame

Now that the node is up and running, we might as well try sending an ethernet frame over the virtual network.

#### Receiving a VIRTUAL packet

Assuming your message sent over the virtual network is to be echoed, here's how you'll capture it:

#### Shutting down the Node