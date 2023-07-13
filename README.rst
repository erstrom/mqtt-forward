
mqtt-forward
============

Tool used to forward TCP traffic (typically SSH) over MQTT.

The tool has been developed with SSH in mind, but any TCP connection can be forwarded.

Below picture shows how the tool is supposed to be used:

::

                                       _________
                                      (      )  )
                                    ( MQTT broker )
                                   (___)___________)
      Network A                      /            \                     Network B
     --------------     -----------------     -----------------     --------------
     | TCP client | <-> | mqtt-forward  |     | mqtt-forward  | <-> | TCP server |
     |            |     | (client side) |     | (server side) |     |            |
     --------------     -----------------     -----------------     --------------

A TCP client (on network A) wants to connect to a remote TCP server on a remote network (network B)
The only connection between the two networks is via an MQTT broker, so the client can't connect directly to the server on network B.
Two instances of mqtt-forward is run on each network, both connected to the same MQTT broker.

The server side instance will act as a TCP client and will connect to the TCP server on network B.
All data received from MQTT will be sent to the server on the local connection.
The client side instance will open a TCP server on network A that the TCP client can connect to.
All data received on the local TCP server will be forwarded to the remote side via MQTT.

Simple usage example
--------------------

Here is an example of how to use mqtt-forward to forward ssh traffic between two networks.

On network B, run mqtt-forward in server mode::

    mqtt-forward --server --port 22 --mqtt-host path.to.mqtt-broker --server-side-id my-server-id

On network A, run mqtt-forward in client mode::

    mqtt-forward --port 1234 --client-id my-client --mqtt-host path.to.mqtt-broker --server-side-id my-server-id

On network A, run the ssh client::

    ssh -p 1234 user@localhost

If mqtt-forward is not run on the same computer as the ssh client on network A, the address used with the ssh client should of course be the IP address of the machine running mqtt-forward.

Note that both client and server side mqtt-forward instances must use the same --server-side-id in order to establish a connection!

Secure connection example
-------------------------

Here is an example with TLS security.

On network B, run mqtt-forward in server mode::

    mqtt-forward --server --port 22 --mqtt-host path.to.mqtt-broker --server-side-id my-server-id \
        --tls \
        --mqtt-root-ca root_ca.pem \
        --mqtt-certificate cert.pem \
        --mqtt-private-key private_key.pem

On network A, run mqtt-forward in client mode::

    mqtt-forward --port 1234 --client-id my-client --mqtt-host path.to.mqtt-broker --server-side-id my-server-id \
        --tls \
        --mqtt-root-ca root_ca.pem \
        --mqtt-certificate cert.pem \
        --mqtt-private-key private_key.pem

On network A, run the ssh client::

    ssh -p 1234 user@localhost

Build
-----

mqtt-forward requires both mosquitto library and openssl

Build with cmake::

    mkdir build
    cd build
    cmake ..
    make
    cd -

