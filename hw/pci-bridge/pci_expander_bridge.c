/*
 * PCI Expander Bridge Device Emulation
 *
 * Copyright (C) 2015 Red Hat Inc
 *
 * Authors:
 *   Marcel Apfelbaum <marcel@redhat.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_bus.h"
#include "hw/pci/pci_host.h"
#include "hw/qdev-properties.h"
#include "hw/pci/pci_bridge.h"
#include "hw/cxl/cxl.h"
#include "qemu/range.h"
#include "qemu/error-report.h"
#include "qemu/module.h"
#include "sysemu/numa.h"
#include "hw/boards.h"
#include "qom/object.h"

enum BusType { PCI, PCIE, CXL };

#define TYPE_PXB_BUS "pxb-bus"
typedef struct PXBBus PXBBus;
DECLARE_INSTANCE_CHECKER(PXBBus, PXB_BUS,
                         TYPE_PXB_BUS)

#define TYPE_PXB_PCIE_BUS "pxb-pcie-bus"
DECLARE_INSTANCE_CHECKER(PXBBus, PXB_PCIE_BUS,
                         TYPE_PXB_PCIE_BUS)

#define TYPE_PXB_CXL_BUS "pxb-cxl-bus"
DECLARE_INSTANCE_CHECKER(PXBBus, PXB_CXL_BUS,
                         TYPE_PXB_CXL_BUS)

struct PXBBus {
    /*< private >*/
    PCIBus parent_obj;
    /*< public >*/

    char bus_path[8];
};

#define TYPE_PXB_DEVICE "pxb"
typedef struct PXBDev PXBDev;
DECLARE_INSTANCE_CHECKER(PXBDev, PXB_DEV,
                         TYPE_PXB_DEVICE)

#define TYPE_PXB_PCIE_DEVICE "pxb-pcie"
DECLARE_INSTANCE_CHECKER(PXBDev, PXB_PCIE_DEV,
                         TYPE_PXB_PCIE_DEVICE)

typedef struct CXLHost {
    PCIHostState parent_obj;

    CXLComponentState cxl_cstate;
    PXBDev *dev;
} CXLHost;

static PXBDev *convert_to_pxb(PCIDevice *dev)
{
    /* A CXL PXB's parent bus is PCIe, so the normal check won't work */
    if (object_dynamic_cast(OBJECT(dev), TYPE_PXB_CXL_DEVICE)) {
        return PXB_CXL_DEV(dev);
    }

    return pci_bus_is_express(pci_get_bus(dev))
        ? PXB_PCIE_DEV(dev) : PXB_DEV(dev);
}

static GList *pxb_dev_list;

#define TYPE_PXB_HOST "pxb-host"

#define TYPE_PXB_CXL_HOST "pxb-cxl-host"
#define PXB_CXL_HOST(obj) OBJECT_CHECK(CXLHost, (obj), TYPE_PXB_CXL_HOST)

static int pxb_bus_num(PCIBus *bus)
{
    PXBDev *pxb = convert_to_pxb(bus->parent_dev);

    return pxb->bus_nr;
}

static uint16_t pxb_bus_numa_node(PCIBus *bus)
{
    PXBDev *pxb = convert_to_pxb(bus->parent_dev);

    return pxb->numa_node;
}

static int32_t pxb_bus_uid(PCIBus *bus)
{
    PXBDev *pxb = convert_to_pxb(bus->parent_dev);

    return pxb->uid;
}

static void pxb_bus_class_init(ObjectClass *class, void *data)
{
    PCIBusClass *pbc = PCI_BUS_CLASS(class);

    pbc->bus_num = pxb_bus_num;
    pbc->numa_node = pxb_bus_numa_node;
    pbc->uid = pxb_bus_uid;
}

static const TypeInfo pxb_bus_info = {
    .name          = TYPE_PXB_BUS,
    .parent        = TYPE_PCI_BUS,
    .instance_size = sizeof(PXBBus),
    .class_init    = pxb_bus_class_init,
};

static const TypeInfo pxb_pcie_bus_info = {
    .name          = TYPE_PXB_PCIE_BUS,
    .parent        = TYPE_PCIE_BUS,
    .instance_size = sizeof(PXBBus),
    .class_init    = pxb_bus_class_init,
};

static const TypeInfo pxb_cxl_bus_info = {
    .name          = TYPE_PXB_CXL_BUS,
    .parent        = TYPE_CXL_BUS,
    .instance_size = sizeof(PXBBus),
    .class_init    = pxb_bus_class_init,
};

static const char *pxb_host_root_bus_path(PCIHostState *host_bridge,
                                          PCIBus *rootbus)
{
    PXBBus *bus = pci_bus_is_cxl(rootbus) ?
                      PXB_CXL_BUS(rootbus) :
                      pci_bus_is_express(rootbus) ? PXB_PCIE_BUS(rootbus) :
                                                    PXB_BUS(rootbus);

    snprintf(bus->bus_path, 8, "0000:%02x", pxb_bus_num(rootbus));
    return bus->bus_path;
}

static char *pxb_host_ofw_unit_address(const SysBusDevice *dev)
{
    const PCIHostState *pxb_host;
    const PCIBus *pxb_bus;
    const PXBDev *pxb_dev;
    int position;
    const DeviceState *pxb_dev_base;
    const PCIHostState *main_host;
    const SysBusDevice *main_host_sbd;

    pxb_host = PCI_HOST_BRIDGE(dev);
    pxb_bus = pxb_host->bus;
    pxb_dev = convert_to_pxb(pxb_bus->parent_dev);
    position = g_list_index(pxb_dev_list, pxb_dev);
    assert(position >= 0);

    pxb_dev_base = DEVICE(pxb_dev);
    main_host = PCI_HOST_BRIDGE(pxb_dev_base->parent_bus->parent);
    main_host_sbd = SYS_BUS_DEVICE(main_host);

    if (main_host_sbd->num_mmio > 0) {
        return g_strdup_printf(TARGET_FMT_plx ",%x",
                               main_host_sbd->mmio[0].addr, position + 1);
    }
    if (main_host_sbd->num_pio > 0) {
        return g_strdup_printf("i%04x,%x",
                               main_host_sbd->pio[0], position + 1);
    }
    return NULL;
}

static void pxb_host_class_init(ObjectClass *class, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(class);
    SysBusDeviceClass *sbc = SYS_BUS_DEVICE_CLASS(class);
    PCIHostBridgeClass *hc = PCI_HOST_BRIDGE_CLASS(class);

    dc->fw_name = "pci";
    /* Reason: Internal part of the pxb/pxb-pcie device, not usable by itself */
    dc->user_creatable = false;
    sbc->explicit_ofw_unit_address = pxb_host_ofw_unit_address;
    hc->root_bus_path = pxb_host_root_bus_path;
}

static const TypeInfo pxb_host_info = {
    .name          = TYPE_PXB_HOST,
    .parent        = TYPE_PCI_HOST_BRIDGE,
    .class_init    = pxb_host_class_init,
};

static void pxb_cxl_realize(DeviceState *dev, Error **errp)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);
    PCIHostState *phb = PCI_HOST_BRIDGE(dev);
    CXLHost *cxl = PXB_CXL_HOST(dev);
    struct cxl_dev *cxl_dev = &cxl->dev->cxl;
    CXLComponentState *cxl_cstate = &cxl->cxl_cstate;
    struct MemoryRegion *mr = &cxl_cstate->crb.component_registers;
    int uid = pci_bus_uid(phb->bus);

    cxl_component_register_block_init(OBJECT(dev), cxl_cstate,
                                      TYPE_PXB_CXL_HOST);
    sysbus_init_mmio(sbd, mr);

    sysbus_mmio_map(sbd, 0, CXL_HOST_BASE + memory_region_size(mr) * uid);

    /*
     * A CXL host bridge can exist without a fixed memory window, but it would
     * only operate in legacy PCIe mode.
     */
    if (!cxl_dev->memory_window[0]) {
        warn_report(
            "CXL expander bridge created without window. Consider using %s",
            "memdev[0]=<memory_backend>");
        return;
    }

    /*
     * FIXME: Use the first window for this host bridge. A host bridge should
     * enable all windows.
     */
    mr = host_memory_backend_get_memory(cxl_dev->memory_window[0]);
    sysbus_init_mmio(sbd, mr);
    sysbus_mmio_map(sbd, 1, *cxl_dev->window_base[0]);
}

static void pxb_cxl_host_class_init(ObjectClass *class, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(class);
    PCIHostBridgeClass *hc = PCI_HOST_BRIDGE_CLASS(class);

    hc->root_bus_path = pxb_host_root_bus_path;
    dc->fw_name = "cxl";
    dc->realize = pxb_cxl_realize;
    /* Reason: Internal part of the pxb/pxb-pcie device, not usable by itself */
    dc->user_creatable = false;
}

/*
 * This is a device to handle the MMIO for a CXL host bridge. It does nothing
 * else.
 */
static const TypeInfo cxl_host_info = {
    .name          = TYPE_PXB_CXL_HOST,
    .parent        = TYPE_PCI_HOST_BRIDGE,
    .instance_size = sizeof(CXLHost),
    .class_init    = pxb_cxl_host_class_init,
};

/*
 * Registers the PXB bus as a child of pci host root bus.
 */
static void pxb_register_bus(PCIDevice *dev, PCIBus *pxb_bus, Error **errp)
{
    PCIBus *bus = pci_get_bus(dev);
    int pxb_bus_num = pci_bus_num(pxb_bus);

    if (bus->parent_dev) {
        error_setg(errp, "PXB devices can be attached only to root bus");
        return;
    }

    QLIST_FOREACH(bus, &bus->child, sibling) {
        if (pci_bus_num(bus) == pxb_bus_num) {
            error_setg(errp, "Bus %d is already in use", pxb_bus_num);
            return;
        }
    }
    QLIST_INSERT_HEAD(&pci_get_bus(dev)->child, pxb_bus, sibling);
}

static int pxb_map_irq_fn(PCIDevice *pci_dev, int pin)
{
    PCIDevice *pxb = pci_get_bus(pci_dev)->parent_dev;

    /*
     * The bios does not index the pxb slot number when
     * it computes the IRQ because it resides on bus 0
     * and not on the current bus.
     * However QEMU routes the irq through bus 0 and adds
     * the pxb slot to the IRQ computation of the PXB
     * device.
     *
     * Synchronize between bios and QEMU by canceling
     * pxb's effect.
     */
    return pin - PCI_SLOT(pxb->devfn);
}

static void pxb_dev_reset(DeviceState *dev)
{
    CXLHost *cxl = PXB_CXL_HOST(dev);
    CXLComponentState *cxl_cstate = &cxl->cxl_cstate;
    uint32_t *reg_state = cxl_cstate->crb.cache_mem_registers;

    cxl_component_register_init_common(reg_state, CXL2_ROOT_PORT);
}

static gint pxb_compare(gconstpointer a, gconstpointer b)
{
    const PXBDev *pxb_a = a, *pxb_b = b;

    return pxb_a->bus_nr < pxb_b->bus_nr ? -1 :
           pxb_a->bus_nr > pxb_b->bus_nr ?  1 :
           0;
}

static void pxb_dev_realize_common(PCIDevice *dev, enum BusType type,
                                   Error **errp)
{
    PXBDev *pxb = convert_to_pxb(dev);
    DeviceState *ds, *bds = NULL;
    PCIBus *bus;
    const char *dev_name = NULL;
    Error *local_err = NULL;
    MachineState *ms = MACHINE(qdev_get_machine());

    if (ms->numa_state == NULL) {
        error_setg(errp, "NUMA is not supported by this machine-type");
        return;
    }

    if (pxb->numa_node != NUMA_NODE_UNASSIGNED &&
        pxb->numa_node >= ms->numa_state->num_nodes) {
        error_setg(errp, "Illegal numa node %d", pxb->numa_node);
        return;
    }

    if (dev->qdev.id && *dev->qdev.id) {
        dev_name = dev->qdev.id;
    }

    ds = qdev_new(type == CXL ? TYPE_PXB_CXL_HOST : TYPE_PXB_HOST);
    if (type == PCIE) {
        bus = pci_root_bus_new(ds, dev_name, NULL, NULL, 0, TYPE_PXB_PCIE_BUS);
    } else if (type == CXL) {
        bus = pci_root_bus_new(ds, dev_name, NULL, NULL, 0, TYPE_PXB_CXL_BUS);
        bus->flags |= PCI_BUS_CXL;
        PXB_CXL_HOST(ds)->dev = PXB_CXL_DEV(dev);
        PXB_CXL_DEV(dev)->cxl.cxl_host_bridge = ds;
    } else {
        bus = pci_root_bus_new(ds, "pxb-internal", NULL, NULL, 0, TYPE_PXB_BUS);
        bds = qdev_new("pci-bridge");
        bds->id = dev_name;
        qdev_prop_set_uint8(bds, PCI_BRIDGE_DEV_PROP_CHASSIS_NR, pxb->bus_nr);
        qdev_prop_set_bit(bds, PCI_BRIDGE_DEV_PROP_SHPC, false);
    }

    bus->parent_dev = dev;
    bus->address_space_mem = pci_get_bus(dev)->address_space_mem;
    bus->address_space_io = pci_get_bus(dev)->address_space_io;
    bus->map_irq = pxb_map_irq_fn;

    PCI_HOST_BRIDGE(ds)->bus = bus;

    pxb_register_bus(dev, bus, &local_err);
    if (local_err) {
        error_propagate(errp, local_err);
        goto err_register_bus;
    }

    sysbus_realize_and_unref(SYS_BUS_DEVICE(ds), &error_fatal);
    if (bds) {
        qdev_realize_and_unref(bds, &bus->qbus, &error_fatal);
    }

    pci_word_test_and_set_mask(dev->config + PCI_STATUS,
                               PCI_STATUS_66MHZ | PCI_STATUS_FAST_BACK);
    pci_config_set_class(dev->config, PCI_CLASS_BRIDGE_HOST);

    pxb_dev_list = g_list_insert_sorted(pxb_dev_list, pxb, pxb_compare);

    pxb_dev_reset(ds);

    return;

err_register_bus:
    object_unref(OBJECT(bds));
    object_unparent(OBJECT(bus));
    object_unref(OBJECT(ds));
}

static void pxb_dev_realize(PCIDevice *dev, Error **errp)
{
    if (pci_bus_is_express(pci_get_bus(dev))) {
        error_setg(errp, "pxb devices cannot reside on a PCIe bus");
        return;
    }

    pxb_dev_realize_common(dev, PCI, errp);
}

static void pxb_dev_exitfn(PCIDevice *pci_dev)
{
    PXBDev *pxb = convert_to_pxb(pci_dev);

    pxb_dev_list = g_list_remove(pxb_dev_list, pxb);
}

static Property pxb_dev_properties[] = {
    /* Note: 0 is not a legal PXB bus number. */
    DEFINE_PROP_UINT8("bus_nr", PXBDev, bus_nr, 0),
    DEFINE_PROP_UINT16("numa_node", PXBDev, numa_node, NUMA_NODE_UNASSIGNED),
    DEFINE_PROP_INT32("uid", PXBDev, uid, -1),
    DEFINE_PROP_ARRAY("window-base", PXBDev, cxl.num_windows, cxl.window_base,
                      qdev_prop_uint64, hwaddr),
    DEFINE_PROP_END_OF_LIST(),
};

static void pxb_dev_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->realize = pxb_dev_realize;
    k->exit = pxb_dev_exitfn;
    k->vendor_id = PCI_VENDOR_ID_REDHAT;
    k->device_id = PCI_DEVICE_ID_REDHAT_PXB;
    k->class_id = PCI_CLASS_BRIDGE_HOST;

    dc->desc = "PCI Expander Bridge";
    device_class_set_props(dc, pxb_dev_properties);
    dc->hotpluggable = false;
    set_bit(DEVICE_CATEGORY_BRIDGE, dc->categories);

    /*
     * Reset doesn't seem to actually be called, but maybe it will in the
     * future?
     */
    dc->reset = pxb_dev_reset;
}

static const TypeInfo pxb_dev_info = {
    .name          = TYPE_PXB_DEVICE,
    .parent        = TYPE_PCI_DEVICE,
    .instance_size = sizeof(PXBDev),
    .class_init    = pxb_dev_class_init,
    .interfaces = (InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static void pxb_pcie_dev_realize(PCIDevice *dev, Error **errp)
{
    if (!pci_bus_is_express(pci_get_bus(dev))) {
        error_setg(errp, "pxb-pcie devices cannot reside on a PCI bus");
        return;
    }

    pxb_dev_realize_common(dev, PCIE, errp);
}

static void pxb_pcie_dev_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->realize = pxb_pcie_dev_realize;
    k->exit = pxb_dev_exitfn;
    k->vendor_id = PCI_VENDOR_ID_REDHAT;
    k->device_id = PCI_DEVICE_ID_REDHAT_PXB_PCIE;
    k->class_id = PCI_CLASS_BRIDGE_HOST;

    dc->desc = "PCI Express Expander Bridge";
    device_class_set_props(dc, pxb_dev_properties);
    dc->hotpluggable = false;
    set_bit(DEVICE_CATEGORY_BRIDGE, dc->categories);
}

static const TypeInfo pxb_pcie_dev_info = {
    .name          = TYPE_PXB_PCIE_DEVICE,
    .parent        = TYPE_PCI_DEVICE,
    .instance_size = sizeof(PXBDev),
    .class_init    = pxb_pcie_dev_class_init,
    .interfaces = (InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static void pxb_cxl_dev_realize(PCIDevice *dev, Error **errp)
{
    PXBDev *pxb = PXB_CXL_DEV(dev);
    struct cxl_dev *cxl = &pxb->cxl;
    int count = 0;

    /* A CXL PXB's parent bus is still PCIe */
    if (!pci_bus_is_express(pci_get_bus(dev))) {
        error_setg(errp, "pxb-cxl devices cannot reside on a PCI bus");
        return;
    }

    if (pxb->uid < 0) {
        error_setg(errp, "pxb-cxl devices must have a valid uid (0-2147483647)");
        return;
    }

    /* FIXME: Check that uid doesn't collide with UIDs of other host bridges */

    pxb_dev_realize_common(dev, CXL, errp);

    for (unsigned i = 0; i < CXL_WINDOW_MAX; i++) {
        if (!cxl->memory_window[i]) {
            continue;
        }

        count++;
    }

    if (!count) {
        warn_report("memory-windows should be set when creating CXL host bridges");
    }

    if (count != cxl->num_windows) {
        error_setg(errp, "window bases count (%d) must match window count (%d)",
                   cxl->num_windows, count);
    }
}

static void pxb_cxl_dev_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc   = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->realize             = pxb_cxl_dev_realize;
    k->exit                = pxb_dev_exitfn;
    /*
     * XXX: These types of bridges don't actually show up in the hierarchy so
     * vendor, device, class, etc. ids are intentionally left out.
     */

    dc->desc = "CXL Host Bridge";
    device_class_set_props(dc, pxb_dev_properties);
    set_bit(DEVICE_CATEGORY_BRIDGE, dc->categories);

    /* Host bridges aren't hotpluggable. FIXME: spec reference */
    dc->hotpluggable = false;

    /*
     * Below is moral equivalent of:
     *   DEFINE_PROP_ARRAY("memdev", PXBDev, window_count, windows,
     *                     qdev_prop_memory_device, HostMemoryBackend)
     */
    for (unsigned i = 0; i < CXL_WINDOW_MAX; i++) {
        g_autofree char *name = g_strdup_printf("memdev[%u]", i);
        object_class_property_add_link(klass, name, TYPE_MEMORY_BACKEND,
                offsetof(PXBDev, cxl.memory_window[i]),
                qdev_prop_allow_set_link_before_realize,
                OBJ_PROP_LINK_STRONG);
    }
}

static const TypeInfo pxb_cxl_dev_info = {
    .name          = TYPE_PXB_CXL_DEVICE,
    .parent        = TYPE_PCI_DEVICE,
    .instance_size = sizeof(PXBDev),
    .class_init    = pxb_cxl_dev_class_init,
    .interfaces =
        (InterfaceInfo[]){
            { INTERFACE_CONVENTIONAL_PCI_DEVICE },
            {},
        },
};

static void pxb_register_types(void)
{
    type_register_static(&pxb_bus_info);
    type_register_static(&pxb_pcie_bus_info);
    type_register_static(&pxb_cxl_bus_info);
    type_register_static(&pxb_host_info);
    type_register_static(&cxl_host_info);
    type_register_static(&pxb_dev_info);
    type_register_static(&pxb_pcie_dev_info);
    type_register_static(&pxb_cxl_dev_info);
}

type_init(pxb_register_types)
