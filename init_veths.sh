apt install iproute2 iperf libpcap-dev gdb ping iperf3
ip netns add red
ip link add veth0 type veth peer name veth0-1
ip link add veth1 type veth peer name veth1-1
ip link set veth1-1 netns red


ip addr add 10.1.1.1/24 dev veth0-1
ip link set dev veth0-1 up
ip link set dev veth0 up
ip link set dev veth1 up
ip netns exec red ip addr add 10.1.1.2/24 dev veth1-1
ip netns exec red ip link set dev veth1-1 up

#term 3: ip netns exec red iperf -s 10.1.1.2
#term 2: iperf -c 10.1.1.2
#term 1: t4p4s
