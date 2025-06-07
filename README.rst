
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

The tool supports packet re-ordering and retransmissions in order to have reliable communication between both peers.
This is especially important for MQTT QoS levels 0 and 1.
MQTT does not guarantee that packets are being received in the same order they are sent, even at QoS level 2.

Simple usage example
--------------------

Here is an example of how to use mqtt-forward to forward ssh traffic between two networks.

On network B, run mqtt-forward in server mode::

    mqtt-forward --server --port 22 --mqtt-host path.to.mqtt-broker --server-side-id my-server-id

On network A, run mqtt-forward in client mode::

    mqtt-forward --port 1234 --mqtt-host path.to.mqtt-broker --server-side-id my-server-id

On network A, run the ssh client::

    ssh -p 1234 user@localhost

If mqtt-forward is not run on the same computer as the ssh client on network A, the address used with the ssh client should of course be the IP address of the machine running mqtt-forward.

Note that both client and server side mqtt-forward instances must use the same --server-side-id in order to establish a connection!

For more detailed usage examples, se section *Usage examples* below.

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

    mqtt-forward --port 1234 --mqtt-host path.to.mqtt-broker --server-side-id my-server-id \
        --tls \
        --mqtt-root-ca root_ca.pem \
        --mqtt-certificate cert.pem \
        --mqtt-private-key private_key.pem

On network A, run the ssh client::

    ssh -p 1234 user@localhost

In the test directory, there are scripts for both mqtt-forward startup as well as the setup of a mosquitto server.
These scripts can be used as an example of how to use the tool.

Using environment variables
+++++++++++++++++++++++++++

Environment variables can be used to configure some of the settings.
This is useful in order to avoid having to type that much on the command line.

Here is an example. Variables have been exported in a text file::

    cat mqtt-forward-env
    export MQTT_FORWARD_MQTT_HOST=path.to.mqtt-broker
    export MQTT_FORWARD_ROOT_CA=/path/to/root_ca.pem
    export MQTT_FORWARD_CERTIFICATE=/path/to/cert.pem
    export MQTT_FORWARD_PRIVATE_KEY=/path/to/private_key.pem

Source the environment setup file once and run the program::

    . mqtt-forward-env
    mqtt-forward --server-side-id my-server-id -p 1234

Build
-----

mqtt-forward requires both mosquitto library and openssl

On debian and ubuntu, the necessary packets needed for building can be installed like this::

    sudo apt install libmosquitto-dev libssl-dev

Build with cmake::

    mkdir build
    cd build
    cmake ..
    make
    sudo make install
    cd -

Usage examples
--------------

This section contains some more detailed examples of how to use mqtt-forward.

Setup unsecure MQTT broker on debian/ubuntu
+++++++++++++++++++++++++++++++++++++++++++

In order to use mqtt-forward there must be an mqtt broker available that the program can connect to.

All below commands should be run as root user.

Install the mosquitto broker like this::

    apt install mosquitto

Update the config to allow anonymous access::

    cat > /etc/mosquitto/conf.d/mosquitto.conf <<- EOM
    listener 1883
    require_certificate false
    allow_anonymous true
    EOM

Restart mosquitto in order to make the new settings effective::

    systemctl restart mosquitto

Setup server side program for SSH access
++++++++++++++++++++++++++++++++++++++++

Let's assume that the broker that was installed in the previous example has a public IP address and is accessible over internet.
Let's also assume that it was installed on a computer with domain name *some-domain.se*

We have a computer on a private NAT network that we want to have SSH access to.
Since the computer is on a NAT network it does not have a public IP address of its own and thus, it can't be accessed from the internet
(programs on the computer can only connect to servers on the internet).

mqtt-forward solves this by tunneling the TCP traffic via the mqtt broker on some-domain.se

Build mqtt-forward according to the build instructions above and install on the computer on the private network we want to have SSH access to.

We want to have mqtt-forward running as a service in the background, so we create a systemd unit file.

Here is an example::

    cat > /etc/systemd/system/mqtt-forward.service <<- EOM
    [Unit]
    Description=mqtt-forward
    After=network-online.target

    [Service]
    User=1000
    Group=1000
    ExecStart=/usr/bin/mqtt-forward --mqtt-host some-domain.se --server-side-id my-server-id -s -b
    # Automatically restart the service if it crashes
    Restart=on-failure
    Type=simple

    [Install]

    # Tell systemd to automatically start this service when the system boots
    # (assuming the service is enabled)
    WantedBy=multi-user.target
    EOM

The above unit file will start an instance of mqtt-forward with server side id "my-server-id".
It will connect to MQTT broker some-domain.se on port 1883. The port was not specified on the command line since it is the default port.
The -b flag tells mqtt-forward to broadcast its precence to the broker so clients can detect if it is available.
The -s flag is used to run the program in server mode.
Since no address or port options were specified, default values will be used.
These are "127.0.0.1" for the address and "22" for the port.
This means that incoming TCP traffic will be forwarded to port 22 (SSH server) on the same machine as the service is running on.

Enable and start the service on the computer::

    systemctl --system daemon-reload
    systemctl enable mqtt-forward.service
    systemctl start mqtt-forward.service

Connect to a server using mqtt-forward
++++++++++++++++++++++++++++++++++++++

If a server side program was launched with the *-b* option, it will transmit beacons which makes it easier for clients to know if it is available.

On the computer from where the connection to the remote server is going to be established,
run mqtt-forward with the *-l* option in order to list all available servers::

    mqtt-forward --mqtt-host some-domain.se -l

If the server created in the above example is available, a print similar to the one below will be shown::


    Detected servers:

                                 Server ID       Last seen (seconds ago)

                              my-server-id                             0

To connect to "my-server-id", run mqtt-forward like this::

    mqtt-forward --mqtt-host some-domain.se --server-side-id my-server-id -p 1234

This will start an mqtt-forward instance that will connect to MQTT broker some-domain.se and create a tunnel to server "my-server-id".
It will listen to incoming TCP connections on port 1234 and forward all traffic to the server "my-server-id".

Had we not provided the -p|--port argument, the default port (22) would have been used.
This would work if there is no native SSH server running on the same computer and if the user has privilege to bind to port 22.
But it is recommended to use another port than 22 on the client side.

It is now possible to connect to the remote server like this (from the same computer where mqtt-forward is running)::

    ssh -p 1234 user@localhost

From another computer on the same local network as the computer hosting mqtt-forward::

    ssh -p 1234 user@<ip address of computer hosting mqtt-forward>

Test
----

The **test** directory contains a docker based test framework that can be used to test SSH forwarding.

Both secure and unsecure setups are supported.
With the secure setup, the MQTT broker will only accept connections from clients with certificates signed by the same CA as the broker certificate.
With the unsecure setup, all TLS security have been disabled and the broker will accept all clients.

The test scripts and docker-compose file can also be used as an example of how to setup mqtt-forward together with the mosquitto mqtt broker.

The test framework is comprised of one mosquitto instance running in a docker container and two instances of mqtt-forward.
The mqtt-forward instances are setup for SSH forwarding. Both instances will connect to the same broker.

Make sure the tool is built prior to running the tests!

The test framework can be started from one script::

    ./test/start-all.sh [-u|--unsecure]

The script takes one command line option: **-u|--unsecure**.
If set, an unsecure test setup will be created and started.

The default is to have a secure setup.

In a secure setup, a certificate chain will be created for both mqtt-forward instances as well as the broker.
All certificates (broker as well as client certificates) will be signed by a common self signed CA certificate.

An instance of the **eclipse-mosquitto** docker image will be launched and configured to use the generated certificates.

Two instances of mqtt-forward will also be started.
The server side instance will forward all incoming traffic to port 22 on localhost
The client side instance will listen on port 1230.

Use the ssh client to test the forwarding::

    ssh -p 1230 user@localhost

To stop the mosquitto docker image and the mqtt-forward instances, run::

    ./test/teardown.sh
