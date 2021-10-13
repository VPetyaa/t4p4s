apt install vim libpcap-dev iproute2 iperf libpcap-dev gdb iputils-ping iperf3 tmux ethtool -y
ip netns add red
ip netns add blue
ip link add veth0 type veth peer name veth0-1 netns blue
ip link add veth1 type veth peer name veth1-1 netns red
#ip link set veth1-1 netns red


ip netns exec blue ip addr add 10.10.10.1/24 dev veth0-1
ip netns exec blue ip link set dev veth0-1 up
ip netns exec red ip addr add 10.10.10.2/24 dev veth1-1
ip netns exec red ip link set dev veth1-1 up

ip link set dev veth0 up
ip link set dev veth1 up

ethtool -K veth0 tx off
ip netns exec blue ethtool -K veth0-1 tx off
ethtool -K veth1 tx off
ip netns exec red ethtool -K veth1-1 tx off

#term 3: ip netns exec red iperf -s 10.1.1.2
#term 2: iperf -c 10.1.1.2
#term 1: t4p4s
