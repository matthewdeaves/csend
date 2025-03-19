This repo is a record of my work to learn C programming with the aim of making a peer to peer enabled chat program in C that will run on Ubuntu, macOS and hopefully Mac OS System 6 with a Mac SE.

To start with I am focussing on the Ubuntu and macOS support as that will be the easiest to get back into C programming on with easy to find documentation. However, there is briliant work available to learn from by Joshua Stein such as [https://jcs.org/wikipedia](https://jcs.org/wikipedia) that are using MacTCP etc under System 6 and written in C with the Think C IDE.

I'll be tagging various points of evolution of this code base with a write up of important learnings and aspects of the code as I go.

---

## [v0.0.3](https://github.com/matthewdeaves/csend/tree/v0.0.3)

A rewrite to move to a single binary capable of sending and receiving messages with support for:
* peer to peer discovery over UDP 
* direct and broadcast messaging over TCP
* messages encapsulated with a simple messaging protocol

This is achieved by:

* implementing a [discovery_thread](https://github.com/matthewdeaves/csend/blob/c393d6bf23b3f70750fb68d10c5c1ab0a77cf32b/network.c#L321)
* implementing a [listener_thread](https://github.com/matthewdeaves/csend/blob/c393d6bf23b3f70750fb68d10c5c1ab0a77cf32b/network.c#L224)
* implementing a [user_input_thread](https://github.com/matthewdeaves/csend/blob/c393d6bf23b3f70750fb68d10c5c1ab0a77cf32b/peer.c#L172)
* Moving code from previous versions into [network.c](https://github.com/matthewdeaves/csend/blob/c393d6bf23b3f70750fb68d10c5c1ab0a77cf32b/network.c) with explicit functions to initialise TCP sockets to [listen](https://github.com/matthewdeaves/csend/blob/c393d6bf23b3f70750fb68d10c5c1ab0a77cf32b/network.c#L53) and [send](https://github.com/matthewdeaves/csend/blob/c393d6bf23b3f70750fb68d10c5c1ab0a77cf32b/network.c#L165)
* Implementing a method to [initialise UDP sockets](https://github.com/matthewdeaves/csend/blob/c393d6bf23b3f70750fb68d10c5c1ab0a77cf32b/network.c#L142) to [broadcast discovery](https://github.com/matthewdeaves/csend/blob/c393d6bf23b3f70750fb68d10c5c1ab0a77cf32b/network.c#L142) messages
* Implementing a [main method](https://github.com/matthewdeaves/csend/blob/c393d6bf23b3f70750fb68d10c5c1ab0a77cf32b/peer.c#L310) to start threads
* implementing a very simple structured [message protocol](https://github.com/matthewdeaves/csend/blob/c393d6bf23b3f70750fb68d10c5c1ab0a77cf32b/protocol.c)
* each thead having access to a [`struct app_state_t`](https://github.com/matthewdeaves/csend/blob/c393d6bf23b3f70750fb68d10c5c1ab0a77cf32b/peer.h#L44) to store key information for the peer such as the list of peers and a method to aid in multithreaded access to the list

The code is well commented. Both programs output to the terminal.

Compile with:

```
gcc -o p2p_chat peer.c network.c protocol.c -lpthread
```

Run with:

```
./p2p_chat
```

### Key Learnings

#### Message Protocol

It really is a very simple format which I think in future can be improved to use a more structured format with a struct and introduction of magic numbers at the start of the data and a checksum to help a peer verifiy a connection on a socket has the intended data and was sent by a valid peer. The message format is:

```
// Message format: TYPE|SENDER|CONTENT
// Example: TEXT|username@192.168.1.5|Hello, world!

```
The [parse_message()](https://github.com/matthewdeaves/csend/blob/a4cea91a61c4f70d5ce5de417bf0d7a5a40cc184/protocol.c#L38) function attempts to parse incomming messages according to the above format, 

#### Threads
Each peer maintains an array of network peers within a struct called [app_state_t]() defined in peer.h. This list of peers needs to be modified in a threadsafe manner as a peer can be added to the list via the discovery and listener threads.

To achieve this, app_state_t hold a variable of [pthread_mutex_t]() type called `peers_mutex`. This is a special type which allows the calling of functions to obtain and release a lock on that variable. For a thread to obtain the lock (which blocks execution/waits if it is not available)`pthread_mutex_lock()` is called. To release the lock `pthread_mutex_unlock()` is called. This means you have to remember to make a thread obtain the lock, do your thing and then release the lock when you are done. Both functions take an argument type `pthread_mutex_t`. For example:

```
pthread_mutex_lock(&state->peers_mutex);

// code to do modify the list of peers

pthread_mutex_unlock(&state->peers_mutex);
```

View the [code](https://github.com/matthewdeaves/csend/tree/v0.0.3)

Get the [code](https://github.com/matthewdeaves/csend/releases/tag/v0.0.3)

---

## [v0.0.2](https://github.com/matthewdeaves/csend/tree/v0.0.2)

A minor improvement to the server implementation this time in that it will run continuously listening for connections until the process is terminated (and a graceful shutdown performed). This is achieved by adding:
* [setup](https://github.com/matthewdeaves/csend/blob/58fbc300af851ec4e6b11075ca1ead051a2cb73a/server.c#L54) and [handling](https://github.com/matthewdeaves/csend/blob/58fbc300af851ec4e6b11075ca1ead051a2cb73a/server.c#L24) of SIGINT (Ctrl+C) and SIGTERM signals
* [non-blocking accept](https://github.com/matthewdeaves/csend/blob/58fbc300af851ec4e6b11075ca1ead051a2cb73a/server.c#L108) of connections with a [1 second timeout via select()](https://github.com/matthewdeaves/csend/blob/58fbc300af851ec4e6b11075ca1ead051a2cb73a/server.c#L118). This allows the servier to periodically check if it should continue to run.
* A main server [loop](https://github.com/matthewdeaves/csend/blob/58fbc300af851ec4e6b11075ca1ead051a2cb73a/server.c#L103) which ties the above points together
* A [tidy up before shutdown](https://github.com/matthewdeaves/csend/blob/58fbc300af851ec4e6b11075ca1ead051a2cb73a/server.c#L29), closing the server sockets before program termination

The code is well commented. Both programs output to the terminal.

Compile with:

```
gcc client.c -o client
gcc server.c -o server
```

Run with:

```
./server
```

and in another terminal (run as many of these as you like or repeat whilst server is running):

```
./client
```
View the [code](https://github.com/matthewdeaves/csend/tree/v0.0.2)

Get the [code](https://github.com/matthewdeaves/csend/releases/tag/v0.0.2)

---

## [v0.0.1](https://github.com/matthewdeaves/csend/tree/v0.0.1)

This is a very simple client and server setup using sockets on the localhost. You start the [server](https://github.com/matthewdeaves/csend/blob/v0.0.1/server.c) and it will open a port, bind a socket and  listen for a connections. When a client connects it will read a message, send a response and then close connections and terminate. The call to wait for a connection is [blocking](https://github.com/matthewdeaves/csend/blob/a572ac3b9acbecea0316d38763c26d933245f092/server.c#L57) (as in the program execution pauses until a connection is made on the port).

The [client](https://github.com/matthewdeaves/csend/blob/v0.0.1/client.c) is equally simple, opening a socket, connecting to the localhost, sending a message to the server, reading a response and terminating.

The code is well commented. Both programs output to the terminal.

Compile with:

```
gcc client.c -o client
gcc server.c -o server
```

Run with:

```
./server
```

and in another terminal:

```
./client
```
View the [code](https://github.com/matthewdeaves/csend/tree/v0.0.1)

Get the [code](https://github.com/matthewdeaves/csend/releases/tag/v0.0.1)

---
