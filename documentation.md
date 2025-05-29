# Multicast Snooping and Forwarding Solution

This repository contains the source code for a comprehensive multicast snooping and forwarding solution designed for Linux-based systems. The solution is comprised of two main components:

1.  **`qca-mcs` (Kernel Module):** This is a Linux kernel module responsible for the core multicast snooping and forwarding logic. It inspects network traffic to learn about multicast group memberships and makes intelligent decisions about where to forward multicast packets.

2.  **`qca-mcs-apps` (User-Space Daemon):** This component, typically running as a daemon (`mcsd`), provides user-space control and configuration for the `qca-mcs` kernel module. It interacts with the kernel module to set policies, manage rules, and monitor multicast activity.

Together, these components provide an efficient way to manage multicast traffic within a network, optimizing bandwidth usage and improving overall network performance.

## `qca-mcs` (Kernel Module)

The `qca-mcs` directory houses a Linux kernel module that forms the core of the multicast snooping and forwarding capabilities.

### Implementation Details

-   **Kernel Module Architecture:** `qca-mcs` is implemented as a standard Linux kernel module.
-   **Main Components:**
    -   **Netlink Interface (`mc_netlink.c`, `mc_api.h`):** This component provides the communication channel between the kernel module and user-space applications (like `mcsd`). It uses the Netlink socket protocol for sending and receiving configuration commands and events. `mc_api.h` defines the specific message types and data structures for this interface.
    -   **Netfilter Hooks (`mc_netfilter.c`):** The module utilizes Netfilter hooks to intercept network packets at various points in the Linux networking stack. This allows it to inspect IGMP/MLD messages and multicast data packets.
    -   **Snooping Logic (`mc_snooping.c`):** This is the heart of the module. It processes IGMP (Internet Group Management Protocol) and MLD (Multicast Listener Discovery) messages to learn which hosts are interested in which multicast groups. It maintains a Multicast Database (MDB) to store this membership information.
    -   **Forwarding Logic (`mc_forward.c`):** Based on the information in the MDB, this component makes decisions about where to forward incoming multicast packets. It aims to send multicast traffic only to interfaces where there are active listeners, thus conserving bandwidth.
-   **Bridge and OVS Interaction:** The module is designed to work with the Linux bridging code and also includes support for Open vSwitch (OVS), allowing it to function in various network setups.

### Functionality

-   **IGMP/MLD Snooping:** The primary function is to "snoop" on IGMP (for IPv4) and MLD (for IPv6) messages exchanged between hosts and multicast routers. This allows the module to build a picture of multicast group memberships without actively participating in the routing protocols.
-   **Multicast Database (MDB):** It maintains an MDB that stores information about active multicast groups and the interfaces (ports) on which listeners for those groups reside. This database is crucial for making informed forwarding decisions.
-   **Access Control List (ACL) Support:** `qca-mcs` supports ACL rules to filter multicast traffic based on various criteria (e.g., source/destination IP address, MAC address). This provides a mechanism for controlling which multicast streams are allowed or denied.
-   **Netlink API for User-Space Control:** The module exposes a Netlink-based API (defined in `mc_api.h`) that allows user-space applications to:
    -   Enable or disable the snooping functionality.
    -   Set the multicast forwarding policy (e.g., drop or flood packets for unknown groups).
    -   Add, remove, or flush ACL rules.
    -   Retrieve MDB entries for monitoring and debugging.
    -   Configure various operational parameters like membership intervals, DSCP (Differentiated Services Code Point) retagging values, and router port settings.
    -   Receive asynchronous events, such as notifications when the MDB is updated.
-   **Key Netlink Messages (from `mc_api.h`):**
    -   `MC_MSG_SET_ENABLE`: To enable or disable the snooping feature on a bridge.
    -   `MC_MSG_SET_POLICY`: To define the default forwarding policy for multicast packets.
    -   `MC_MSG_SET_ADD_ACL_RULE`, `MC_MSG_SET_FLUSH_ACL_RULE`: To manage ACL rules.
    -   `MC_MSG_GET_MDB`: To request multicast database entries from the kernel.
    -   `MC_MSG_SET_MEMBERSHIP_INTERVAL`: To configure group membership timers.
    -   `MC_EVENT_MDB_UPDATED`: An event sent from kernel to user-space when the MDB changes.

## `qca-mcs-apps` (User-Space Daemon)

The `qca-mcs-apps` directory contains the user-space application, typically run as a daemon (`mcsd`), that complements the `qca-mcs` kernel module.

### Implementation Details

-   **User-Space Daemon (`mcsd`):** The primary executable is `mcsd`, which runs in the background to manage and control the multicast snooping functionality.
-   **Main Components:**
    -   **`mcsMain.c`:** This file contains the main function and the core application logic for `mcsd`. It handles initialization, command-line argument parsing, signal handling, and the main event loop.
    -   **`mcManager.c`:** This is a crucial component that acts as the bridge between `mcsd` and the `qca-mcs` kernel module. It uses Netlink sockets to send commands to the kernel module (e.g., to enable snooping, set policies, add ACLs) and to receive events (e.g., MDB updates).
-   **Event Loop:** `mcsd` utilizes an event loop (`evloop`) to handle asynchronous operations, such as receiving Netlink messages from the kernel or processing timer events. This allows the daemon to be responsive without consuming excessive system resources.

### Functionality

-   **Kernel Module Configuration and Control:** The primary role of `mcsd` (through `mcManager`) is to configure and control the `qca-mcs` kernel module. It sends Netlink messages to:
    -   Enable or disable snooping on specified bridge interfaces.
    -   Set global parameters like the default forwarding policy.
    -   Add, delete, and manage Access Control Lists (ACLs) for multicast traffic.
-   **Table Management:** `mcManager` is responsible for maintaining and updating various tables that reflect the state of multicast forwarding, including:
    -   Encapsulation tables.
    -   Flood tables.
    -   Interface-specific forwarding tables.
    It retrieves MDB updates from the kernel and processes them to keep these tables current.
-   **Plugin Support and Integration:** The application appears to support a plugin architecture (`pluginManager.c`) and integrates with other specific hardware or software modules, such as:
    -   WLAN managers (`wlanManager.c`, `mcfwdtblwlan2g.c`, `mcfwdtblwlan5g.c`) for wireless interface specific configurations.
    -   PLC (Power Line Communication) manager (`plcManager.c`) for PLC interface specific configurations.
    -   Eswitch (Ethernet Switch) plugin (`mcfwdtbleswitch.c`).
-   **Debugging and Diagnostics:** `mcManager` includes a command-line interface (CLI) accessible via a debug port (`MCS_DBG_PORT`). This CLI provides commands to display internal tables (encapsulation, flood, interface forwarding), which can be useful for debugging and monitoring the state of the multicast snooping system.

## `qca-mcs` Interaction with Application Layer Modules

The `qca-mcs` kernel module is designed to be controlled and configured by user-space applications, primarily the `mcsd` daemon. This interaction is facilitated through a Netlink-based API defined in `mc_api.h`.

User-space applications can leverage this Netlink interface to perform a variety of functions, including:

-   **Enabling/Disabling Snooping:** Start or stop the multicast snooping process on specific bridge interfaces. This is typically one of the first commands sent by `mcsd` to activate the module (e.g., using `MC_MSG_SET_ENABLE`).
-   **Setting Forwarding Policies:** Define the default behavior for multicast packets when group membership is unknown. Policies can include dropping such packets or flooding them to all ports (e.g., using `MC_MSG_SET_POLICY`).
-   **Managing Access Control Lists (ACLs):** Add, remove, or flush ACL rules in the kernel module. These rules allow for fine-grained control over which multicast streams are permitted or denied based on criteria like IP addresses or MAC addresses (e.g., using `MC_MSG_SET_ADD_ACL_RULE`, `MC_MSG_SET_FLUSH_ACL_RULE`).
-   **Retrieving Multicast Database (MDB) Entries:** Request the current MDB from the kernel. This allows `mcsd` and potentially other diagnostic tools to inspect the learned multicast group memberships and the associated interfaces (e.g., using `MC_MSG_GET_MDB`).
-   **Configuring Operational Parameters:** Set various tuning parameters for the snooping process, such as:
    -   Membership intervals (e.g., query intervals, last member query time).
    -   DSCP (Differentiated Services Code Point) values for QoS (Quality of Service) retagging of multicast packets.
    -   Router port configurations.
    -   Timeout behaviors for MDB entries.
-   **Receiving Events:** The kernel module can send asynchronous events to registered user-space applications. A key event is `MC_EVENT_MDB_UPDATED`, which notifies `mcsd` that the multicast database has changed. This prompts `mcManager` to refresh its own tables and potentially update forwarding rules in other modules (like WLAN or PLC managers).

This Netlink interface provides a clear separation between the kernel-level packet processing and the user-level management and policy decisions, allowing for a flexible and extensible multicast control system.
