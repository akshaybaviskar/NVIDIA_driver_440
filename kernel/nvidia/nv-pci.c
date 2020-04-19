/* _NVRM_COPYRIGHT_BEGIN_
 *
 * Copyright 2019 by NVIDIA Corporation.  All rights reserved.  All
 * information contained herein is proprietary and confidential to NVIDIA
 * Corporation.  Any use, reproduction, or disclosure without the written
 * permission of NVIDIA Corporation is prohibited.
 *
 * _NVRM_COPYRIGHT_END_
 */

#include "nv-pci-table.h"
#include "nv-pci.h"
#include "nv-ibmnpu.h"
#include "nv-frontend.h"
#include "nv-msi.h"
#include "nv-hypervisor.h"

#if defined(NV_VGPU_KVM_BUILD)
#include "nv-vgpu-vfio-interface.h"
#endif

nv_cpu_type_t nv_cpu_type = NV_CPU_TYPE_UNKNOWN;

nv_pci_info_t nv_assign_gpu_pci_info[NV_MAX_DEVICES];

static void
nv_pci_validate_assigned_gpus(struct pci_dev *pci_dev)
{
    NvU32 i;

    if (NV_IS_ASSIGN_GPU_PCI_INFO_SPECIFIED())
    {
        for (i = 0; i < nv_assign_gpu_count; i++)
        {
            if ((nv_assign_gpu_pci_info[i].domain == NV_PCI_DOMAIN_NUMBER(pci_dev)) &&
                (nv_assign_gpu_pci_info[i].bus == NV_PCI_BUS_NUMBER(pci_dev)) &&
                (nv_assign_gpu_pci_info[i].slot == NV_PCI_SLOT_NUMBER(pci_dev)))
            {
                nv_assign_gpu_pci_info[i].valid = NV_TRUE;
                return;
            }
        }
    }
}

static void
nv_check_and_blacklist_gpu(
    nvidia_stack_t *sp,
    nv_state_t *nv
)
{
    NV_STATUS rm_status;
    NvU8 *uuid_str;

    rm_status = rm_get_gpu_uuid(sp, nv, &uuid_str, NULL);
    if (rm_status != NV_OK)
    {
        NV_DEV_PRINTF_STATUS(NV_DBG_INFO, nv, rm_status, "Unable to read UUID");
        return;
    }

    if (nv_is_uuid_in_gpu_blacklist(uuid_str))
    {
        rm_status = rm_blacklist_adapter(sp, nv);
        if (rm_status != NV_OK)
        {
            NV_DEV_PRINTF_STATUS(NV_DBG_ERRORS, nv, rm_status,
                          "Failed to blacklist %s", uuid_str);
            goto done;
        }
        nv->flags |= NV_FLAG_BLACKLIST;
        NV_DEV_PRINTF(NV_DBG_INFO, nv, "Blacklisted %s successfully\n",
                      uuid_str);
    }

done:
    os_free_mem(uuid_str);
}

static BOOL nv_treat_missing_irq_as_error(void)
{
#if defined(NV_LINUX_PCIE_MSI_SUPPORTED)
    return (nv_get_hypervisor_type() != OS_HYPERVISOR_HYPERV);
#else
    return TRUE;
#endif
}

static void nv_init_dynamic_power_management
(
    nvidia_stack_t *sp,
    struct pci_dev *pci_dev
)
{
    nv_linux_state_t *nvl = pci_get_drvdata(pci_dev);
    nv_state_t *nv = NV_STATE_PTR(nvl);
    char filename[50];
    int ret;
    NvBool pr3_acpi_method_present = NV_FALSE;

    nvl->sysfs_config_file = NULL;

    ret = snprintf(filename, sizeof(filename),
                   "/sys/bus/pci/devices/%04x:%02x:%02x.0/config",
                   NV_PCI_DOMAIN_NUMBER(pci_dev),
                   NV_PCI_BUS_NUMBER(pci_dev),
                   NV_PCI_SLOT_NUMBER(pci_dev));
    if (ret > 0 || ret < sizeof(filename))
    {
        struct file *file = filp_open(filename, O_RDONLY, 0);
        if (!IS_ERR(file))
        {
            nvl->sysfs_config_file = file;
        }
    }

    if (pci_dev->bus && pci_dev->bus->self)
    {
        pr3_acpi_method_present = nv_acpi_upstreamport_power_resource_method_present(pci_dev->bus->self);
    }

    rm_init_dynamic_power_management(sp, nv, pr3_acpi_method_present);
}


/* find nvidia devices and set initial state */
static int
nv_pci_probe
(
    struct pci_dev *pci_dev,
    const struct pci_device_id *id_table
)
{
    nv_state_t *nv = NULL;
    nv_linux_state_t *nvl = NULL;
    unsigned int i, j;
    int flags = 0;
    nvidia_stack_t *sp = NULL;
    NvBool prev_nv_ats_supported = nv_ats_supported;
    NV_STATUS status;

    nv_printf(NV_DBG_SETUP, "NVRM: probing 0x%x 0x%x, class 0x%x\n",
        pci_dev->vendor, pci_dev->device, pci_dev->class);


































    if (nv_kmem_cache_alloc_stack(&sp) != 0)
    {
        return -1;
    }

    if ((pci_dev->vendor != 0x10de) || (pci_dev->device < 0x20) ||
        ((pci_dev->class != (PCI_CLASS_DISPLAY_VGA << 8)) &&
         (pci_dev->class != (PCI_CLASS_DISPLAY_3D << 8))) ||
        rm_is_legacy_device(sp, pci_dev->device, pci_dev->subsystem_vendor,
                            pci_dev->subsystem_device, FALSE))
    {
        nv_printf(NV_DBG_ERRORS, "NVRM: ignoring the legacy GPU %04x:%02x:%02x.%x\n",
                  NV_PCI_DOMAIN_NUMBER(pci_dev), NV_PCI_BUS_NUMBER(pci_dev),
                  NV_PCI_SLOT_NUMBER(pci_dev), PCI_FUNC(pci_dev->devfn));
        goto failed;
    }

    if (NV_IS_ASSIGN_GPU_PCI_INFO_SPECIFIED())
    {
        for (i = 0; i < nv_assign_gpu_count; i++)
        {
            if (((nv_assign_gpu_pci_info[i].domain == NV_PCI_DOMAIN_NUMBER(pci_dev)) &&
                 (nv_assign_gpu_pci_info[i].bus == NV_PCI_BUS_NUMBER(pci_dev)) &&
                 (nv_assign_gpu_pci_info[i].slot == NV_PCI_SLOT_NUMBER(pci_dev))) &&
                (nv_assign_gpu_pci_info[i].valid))
                break;
        }

        if (i == nv_assign_gpu_count)
        {
            goto failed;
        }
    }

    num_probed_nv_devices++;

    if (pci_enable_device(pci_dev) != 0)
    {
        nv_printf(NV_DBG_ERRORS,
            "NVRM: pci_enable_device failed, aborting\n");
        goto failed;
    }

    if ((pci_dev->irq == 0 && !pci_find_capability(pci_dev, PCI_CAP_ID_MSIX))
        && nv_treat_missing_irq_as_error())
    {
        nv_printf(NV_DBG_ERRORS, "NVRM: Can't find an IRQ for your NVIDIA card!\n");
        nv_printf(NV_DBG_ERRORS, "NVRM: Please check your BIOS settings.\n");
        nv_printf(NV_DBG_ERRORS, "NVRM: [Plug & Play OS] should be set to NO\n");
        nv_printf(NV_DBG_ERRORS, "NVRM: [Assign IRQ to VGA] should be set to YES \n");
        goto failed;
    }

    for (i = 0; i < (NV_GPU_NUM_BARS - 1); i++)
    {
        if (NV_PCI_RESOURCE_VALID(pci_dev, i))
        {
#if defined(NV_PCI_MAX_MMIO_BITS_SUPPORTED)
            if ((NV_PCI_RESOURCE_FLAGS(pci_dev, i) & PCI_BASE_ADDRESS_MEM_TYPE_64) &&
                ((NV_PCI_RESOURCE_START(pci_dev, i) >> NV_PCI_MAX_MMIO_BITS_SUPPORTED)))
            {
                nv_printf(NV_DBG_ERRORS,
                    "NVRM: This is a 64-bit BAR mapped above %dGB by the system\n"
                    "NVRM: BIOS or the %s kernel. This PCI I/O region assigned\n"
                    "NVRM: to your NVIDIA device is not supported by the kernel.\n"
                    "NVRM: BAR%d is %dM @ 0x%llx (PCI:%04x:%02x:%02x.%x)\n",
                    (1 << (NV_PCI_MAX_MMIO_BITS_SUPPORTED - 30)),
                    NV_KERNEL_NAME, i,
                    (NV_PCI_RESOURCE_SIZE(pci_dev, i) >> 20),
                    (NvU64)NV_PCI_RESOURCE_START(pci_dev, i),
                    NV_PCI_DOMAIN_NUMBER(pci_dev),
                    NV_PCI_BUS_NUMBER(pci_dev), NV_PCI_SLOT_NUMBER(pci_dev),
                    PCI_FUNC(pci_dev->devfn));
                goto failed;
            }
#endif
            if ((NV_PCI_RESOURCE_FLAGS(pci_dev, i) & PCI_BASE_ADDRESS_MEM_TYPE_64) &&
                (NV_PCI_RESOURCE_FLAGS(pci_dev, i) & PCI_BASE_ADDRESS_MEM_PREFETCH))
            {
                struct pci_dev *bridge = pci_dev->bus->self;
                NvU32 base_upper, limit_upper;

                if (bridge == NULL)
                    continue;

                pci_read_config_dword(pci_dev, NVRM_PCICFG_BAR_OFFSET(i) + 4,
                        &base_upper);
                if (base_upper == 0)
                    continue;

                pci_read_config_dword(bridge, PCI_PREF_BASE_UPPER32,
                        &base_upper);
                pci_read_config_dword(bridge, PCI_PREF_LIMIT_UPPER32,
                        &limit_upper);

                if ((base_upper != 0) && (limit_upper != 0))
                    continue;

                nv_printf(NV_DBG_ERRORS,
                    "NVRM: This is a 64-bit BAR mapped above 4GB by the system\n"
                    "NVRM: BIOS or the %s kernel, but the PCI bridge\n"
                    "NVRM: immediately upstream of this GPU does not define\n"
                    "NVRM: a matching prefetchable memory window.\n",
                    NV_KERNEL_NAME);
                nv_printf(NV_DBG_ERRORS,
                    "NVRM: This may be due to a known Linux kernel bug.  Please\n"
                    "NVRM: see the README section on 64-bit BARs for additional\n"
                    "NVRM: information.\n");
                goto failed;
            }
                continue;
        }
        nv_printf(NV_DBG_ERRORS,
            "NVRM: This PCI I/O region assigned to your NVIDIA device is invalid:\n"
            "NVRM: BAR%d is %dM @ 0x%llx (PCI:%04x:%02x:%02x.%x)\n", i,
            (NV_PCI_RESOURCE_SIZE(pci_dev, i) >> 20),
            (NvU64)NV_PCI_RESOURCE_START(pci_dev, i),
            NV_PCI_DOMAIN_NUMBER(pci_dev), NV_PCI_BUS_NUMBER(pci_dev),
            NV_PCI_SLOT_NUMBER(pci_dev), PCI_FUNC(pci_dev->devfn));
#if defined(NVCPU_X86)
        if ((NV_PCI_RESOURCE_FLAGS(pci_dev, i) & PCI_BASE_ADDRESS_MEM_TYPE_64) &&
            ((NV_PCI_RESOURCE_START(pci_dev, i) >> PAGE_SHIFT) > 0xfffffULL))
        {
            nv_printf(NV_DBG_ERRORS,
                "NVRM: This is a 64-bit BAR mapped above 4GB by the system\n"
                "NVRM: BIOS or the Linux kernel.  The NVIDIA Linux/x86\n"
                "NVRM: graphics driver and other system software components\n"
                "NVRM: do not support this configuration.\n");
        }
        else
#endif
        if (NV_PCI_RESOURCE_FLAGS(pci_dev, i) & PCI_BASE_ADDRESS_MEM_TYPE_64)
        {
            nv_printf(NV_DBG_ERRORS,
                "NVRM: This is a 64-bit BAR, which some operating system\n"
                "NVRM: kernels and other system software components are known\n"
                "NVRM: to handle incorrectly.  Please see the README section\n"
                "NVRM: on 64-bit BARs for more information.\n");
        }
        else
        {
            nv_printf(NV_DBG_ERRORS,
                "NVRM: The system BIOS may have misconfigured your GPU.\n");
        }
        goto failed;
    }

    if (!request_mem_region(NV_PCI_RESOURCE_START(pci_dev, NV_GPU_BAR_INDEX_REGS),
                            NV_PCI_RESOURCE_SIZE(pci_dev, NV_GPU_BAR_INDEX_REGS),
                            nv_device_name))
    {
        nv_printf(NV_DBG_ERRORS,
            "NVRM: request_mem_region failed for %dM @ 0x%llx. This can\n"
            "NVRM: occur when a driver such as rivatv is loaded and claims\n"
            "NVRM: ownership of the device's registers.\n",
            (NV_PCI_RESOURCE_SIZE(pci_dev, NV_GPU_BAR_INDEX_REGS) >> 20),
            (NvU64)NV_PCI_RESOURCE_START(pci_dev, NV_GPU_BAR_INDEX_REGS));
        goto failed;
    }

    NV_KMALLOC(nvl, sizeof(nv_linux_state_t));
    if (nvl == NULL)
    {
        nv_printf(NV_DBG_ERRORS, "NVRM: failed to allocate memory\n");
        goto err_not_supported;
    }

    os_mem_set(nvl, 0, sizeof(nv_linux_state_t));

    nv  = NV_STATE_PTR(nvl);

    pci_set_drvdata(pci_dev, (void *)nvl);

    /* default to 32-bit PCI bus address space */
    pci_dev->dma_mask = 0xffffffffULL;

    nvl->dev               = &pci_dev->dev;
    nvl->pci_dev           = pci_dev;

    nv->pci_info.vendor_id = pci_dev->vendor;
    nv->pci_info.device_id = pci_dev->device;
    nv->subsystem_id       = pci_dev->subsystem_device;
    nv->subsystem_vendor   = pci_dev->subsystem_vendor;
    nv->os_state           = (void *) nvl;
    nv->pci_info.domain    = NV_PCI_DOMAIN_NUMBER(pci_dev);
    nv->pci_info.bus       = NV_PCI_BUS_NUMBER(pci_dev);
    nv->pci_info.slot      = NV_PCI_SLOT_NUMBER(pci_dev);
    nv->handle             = pci_dev;
    nv->flags             |= flags;

    if (!nv_lock_init_locks(sp, nv))
    {
        goto err_not_supported;
    }

    nvl->all_mappings_revoked = TRUE;
    nvl->safe_to_mmap = TRUE;
    nvl->gpu_wakeup_callback_needed = TRUE;
    INIT_LIST_HEAD(&nvl->open_files);

    for (i = 0, j = 0; i < NVRM_PCICFG_NUM_BARS && j < NV_GPU_NUM_BARS; i++)
    {
        if ((NV_PCI_RESOURCE_VALID(pci_dev, i)) &&
            (NV_PCI_RESOURCE_FLAGS(pci_dev, i) & PCI_BASE_ADDRESS_SPACE)
                == PCI_BASE_ADDRESS_SPACE_MEMORY)
        {
            nv->bars[j].offset = NVRM_PCICFG_BAR_OFFSET(i);
            nv->bars[j].cpu_address = NV_PCI_RESOURCE_START(pci_dev, i);
            nv->bars[j].strapped_size = NV_PCI_RESOURCE_SIZE(pci_dev, i);
            nv->bars[j].size = nv->bars[j].strapped_size;
            j++;
        }
    }
    nv->regs = &nv->bars[NV_GPU_BAR_INDEX_REGS];
    nv->fb   = &nv->bars[NV_GPU_BAR_INDEX_FB];

    nv->interrupt_line = pci_dev->irq;

    nv_init_ibmnpu_info(nv);

#if defined(NVCPU_PPC64LE)
    // Use HW NUMA support as a proxy for ATS support. This is true in the only
    // platform where ATS is currently supported (IBM P9).
    nv_ats_supported &= nv_numa_info_valid(nvl);
#endif

    pci_set_master(pci_dev);

#if defined(CONFIG_VGA_ARB) && !defined(NVCPU_PPC64LE)
#if defined(VGA_DEFAULT_DEVICE)
    vga_tryget(VGA_DEFAULT_DEVICE, VGA_RSRC_LEGACY_MASK);
#endif
    vga_set_legacy_decoding(pci_dev, VGA_RSRC_NONE);
#endif

    if (rm_get_cpu_type(sp, &nv_cpu_type) != NV_OK)
        nv_printf(NV_DBG_ERRORS, "NVRM: error retrieving cpu type\n");

    status = nv_check_gpu_state(nv);
    if (status == NV_ERR_GPU_IS_LOST)
    {
        NV_DEV_PRINTF(NV_DBG_INFO, nv, "GPU is lost, skipping nv_pci_probe\n");
        goto err_not_supported;
    }

    if ((rm_is_supported_device(sp, nv)) != NV_OK)
        goto err_not_supported;

    if (!rm_init_private_state(sp, nv))
    {
        NV_DEV_PRINTF(NV_DBG_ERRORS, nv, "rm_init_private_state() failed!\n");
        goto err_zero_dev;
    }

    nv_printf(NV_DBG_INFO,
              "NVRM: PCI:%04x:%02x:%02x.%x (%04x:%04x): BAR0 @ 0x%llx (%lluMB)\n",
              nv->pci_info.domain, nv->pci_info.bus, nv->pci_info.slot,
              PCI_FUNC(pci_dev->devfn), nv->pci_info.vendor_id, nv->pci_info.device_id,
              nv->regs->cpu_address, (nv->regs->size >> 20));
    nv_printf(NV_DBG_INFO,
              "NVRM: PCI:%04x:%02x:%02x.%x (%04x:%04x): BAR1 @ 0x%llx (%lluMB)\n",
              nv->pci_info.domain, nv->pci_info.bus, nv->pci_info.slot,
              PCI_FUNC(pci_dev->devfn), nv->pci_info.vendor_id, nv->pci_info.device_id,
              nv->fb->cpu_address, (nv->fb->size >> 20));

    num_nv_devices++;

    for (i = 0; i < NV_GPU_NUM_BARS; i++)
    {
        if (nv->bars[i].size != 0)
        {
            if (nv_user_map_register(nv->bars[i].cpu_address,
                nv->bars[i].strapped_size) != 0)
            {
                NV_DEV_PRINTF(NV_DBG_ERRORS, nv,
                              "Failed to register usermap for BAR %u!\n", i);
                for (j = 0; j < i; j++)
                {
                    nv_user_map_unregister(nv->bars[j].cpu_address,
                                           nv->bars[j].strapped_size);
                }
                goto err_zero_dev;
            }
        }
    }

    /*
     * The newly created nvl object is added to the nv_linux_devices global list
     * only after all the initialization operations for that nvl object are
     * completed, so as to protect against simultaneous lookup operations which
     * may discover a partially initialized nvl object in the list
     */
    LOCK_NV_LINUX_DEVICES();

    nv_linux_add_device_locked(nvl);

    UNLOCK_NV_LINUX_DEVICES();

    if (nvidia_frontend_add_device((void *)&nv_fops, nvl) != 0)
        goto err_remove_device;

#if defined(NV_PM_VT_SWITCH_REQUIRED_PRESENT)
    pm_vt_switch_required(nvl->dev, NV_TRUE);
#endif

    nv_procfs_add_gpu(nvl);

#if defined(NV_VGPU_KVM_BUILD)
    if (nvidia_vgpu_vfio_probe(nvl->pci_dev) != NV_OK)
    {
        NV_DEV_PRINTF(NV_DBG_ERRORS, nv, "Failed to register device to vGPU VFIO module");
        nvidia_frontend_remove_device((void *)&nv_fops, nvl);
        goto err_remove_device;
    }
#endif

    nv_check_and_blacklist_gpu(sp, nv);

    nv_init_dynamic_power_management(sp, pci_dev);

#if defined(DPM_FLAG_NEVER_SKIP)
    dev_pm_set_driver_flags(nvl->dev, DPM_FLAG_NEVER_SKIP);
#endif

    nv_kmem_cache_free_stack(sp);

    return 0;

err_remove_device:
    LOCK_NV_LINUX_DEVICES();
    nv_linux_remove_device_locked(nvl);
    UNLOCK_NV_LINUX_DEVICES();
err_zero_dev:
    rm_free_private_state(sp, nv);
err_not_supported:
    nv_ats_supported = prev_nv_ats_supported;
    nv_destroy_ibmnpu_info(nv);
    nv_lock_destroy_locks(sp, nv);
    if (nvl != NULL)
    {
        NV_KFREE(nvl, sizeof(nv_linux_state_t));
    }
    release_mem_region(NV_PCI_RESOURCE_START(pci_dev, NV_GPU_BAR_INDEX_REGS),
                       NV_PCI_RESOURCE_SIZE(pci_dev, NV_GPU_BAR_INDEX_REGS));
    NV_PCI_DISABLE_DEVICE(pci_dev);
    pci_set_drvdata(pci_dev, NULL);
failed:
    nv_kmem_cache_free_stack(sp);
    return -1;
}

static void
nv_pci_remove(struct pci_dev *pci_dev)
{
    nv_linux_state_t *nvl = NULL;
    nv_state_t *nv;
    nvidia_stack_t *sp = NULL;
    NvU32 i;

    nv_printf(NV_DBG_SETUP, "NVRM: removing GPU %04x:%02x:%02x.%x\n",
              NV_PCI_DOMAIN_NUMBER(pci_dev), NV_PCI_BUS_NUMBER(pci_dev),
              NV_PCI_SLOT_NUMBER(pci_dev), PCI_FUNC(pci_dev->devfn));













    if (nv_kmem_cache_alloc_stack(&sp) != 0)
    {
        return;
    }

    LOCK_NV_LINUX_DEVICES();
    nvl = pci_get_drvdata(pci_dev);
    if (!nvl || (nvl->pci_dev != pci_dev))
    {
        goto done;
    }

    nv = NV_STATE_PTR(nvl);

    /*
     * Sanity check: A removed device shouldn't have a non-zero usage_count.
     * For eGPU, fall off the bus along with clients active is a valid scenario.
     * Hence skipping the sanity check for eGPU.
     */
    if ((NV_ATOMIC_READ(nvl->usage_count) != 0) && !(nv->is_external_gpu))
    {
        nv_printf(NV_DBG_ERRORS,
                  "NVRM: Attempting to remove minor device %u with non-zero usage count!\n",
                  nvl->minor_num);
        WARN_ON(1);

        /* We can't continue without corrupting state, so just hang to give the
         * user some chance to do something about this before reboot */
        while (1)
            os_schedule();
    }

    down(&nvl->ldata_lock);

    rm_cleanup_dynamic_power_management(sp, nv);

    rm_check_for_gpu_surprise_removal(sp, nv);

    nv_linux_remove_device_locked(nvl);

    /* Remove proc entry for this GPU */
    nv_procfs_remove_gpu(nvl);

    nv->removed = NV_TRUE;

    UNLOCK_NV_LINUX_DEVICES();

#if defined(NV_PM_VT_SWITCH_REQUIRED_PRESENT)
    pm_vt_switch_unregister(&pci_dev->dev);
#endif

#if defined(NV_VGPU_KVM_BUILD)
    nvidia_vgpu_vfio_remove(pci_dev);
#endif

    /* Update the frontend data structures */
    if (NV_ATOMIC_READ(nvl->usage_count) == 0)
    {
        nvidia_frontend_remove_device((void *)&nv_fops, nvl);
    }

    if ((nv->flags & NV_FLAG_PERSISTENT_SW_STATE) || (nv->flags & NV_FLAG_OPEN))
    {
        if (nv->flags & NV_FLAG_PERSISTENT_SW_STATE)
        {
            rm_disable_gpu_state_persistence(sp, nv);
        }
        nv_shutdown_adapter(sp, nv, nvl);
        nv_dev_free_stacks(nvl);
    }

    if (nvl->sysfs_config_file != NULL)
    {
        filp_close(nvl->sysfs_config_file, NULL);
        nvl->sysfs_config_file = NULL;
    }

    nv_unregister_ibmnpu_devices(nv);
    nv_destroy_ibmnpu_info(nv);

    if (NV_ATOMIC_READ(nvl->usage_count) == 0)
    {
        nv_lock_destroy_locks(sp, nv);
    }

    num_probed_nv_devices--;

    pci_set_drvdata(pci_dev, NULL);

    for (i = 0; i < NV_GPU_NUM_BARS; i++)
    {
        if (nv->bars[i].size != 0)
        {
            nv_user_map_unregister(nv->bars[i].cpu_address,
                                   nv->bars[i].strapped_size);
        }
    }
    rm_i2c_remove_adapters(sp, nv);
    rm_free_private_state(sp, nv);
    release_mem_region(NV_PCI_RESOURCE_START(pci_dev, NV_GPU_BAR_INDEX_REGS),
                       NV_PCI_RESOURCE_SIZE(pci_dev, NV_GPU_BAR_INDEX_REGS));

    num_nv_devices--;

    if (NV_ATOMIC_READ(nvl->usage_count) == 0)
    {
        NV_PCI_DISABLE_DEVICE(pci_dev);
        NV_KFREE(nvl, sizeof(nv_linux_state_t));
    }
    else
    {
        up(&nvl->ldata_lock);
    }

    nv_kmem_cache_free_stack(sp);
    return;

done:
    UNLOCK_NV_LINUX_DEVICES();
    nv_kmem_cache_free_stack(sp);
}

/*!
 * @brief This function accepts pci information corresponding to a GPU
 * and returns a reference to the nv_linux_state_t corresponding to that GPU.
 *
 * @param[in] domain            Pci domain number for the GPU to be found.
 * @param[in] bus               Pci bus number for the GPU to be found.
 * @param[in] slot              Pci slot number for the GPU to be found.
 * @param[in] function          Pci function number for the GPU to be found.
 *
 * @return Pointer to nv_linux_state_t for the GPU if it is found, or NULL otherwise.
 */
nv_linux_state_t * find_pci(NvU32 domain, NvU8 bus, NvU8 slot, NvU8 function)
{
    nv_linux_state_t *nvl = NULL;

    LOCK_NV_LINUX_DEVICES();

    for (nvl = nv_linux_devices; nvl != NULL; nvl = nvl->next)
    {
        nv_state_t *nv = NV_STATE_PTR(nvl);

        if (nv->pci_info.domain == domain &&
            nv->pci_info.bus == bus &&
            nv->pci_info.slot == slot &&
            nv->pci_info.function == function)
        {
            break;
        }
    }

    UNLOCK_NV_LINUX_DEVICES();
    return nvl;
}

int nvidia_dev_get_pci_info(const NvU8 *uuid, struct pci_dev **pci_dev_out,
    NvU64 *dma_start, NvU64 *dma_limit)
{
    nv_linux_state_t *nvl;

    /* Takes nvl->ldata_lock */
    nvl = find_uuid(uuid);
    if (!nvl)
        return -ENODEV;

    *pci_dev_out = nvl->pci_dev;
    *dma_start = nvl->nv_state.dma_addressable_start;
    *dma_limit = nvl->nv_state.dma_addressable_limit;

    up(&nvl->ldata_lock);

    return 0;
}

NvU8 nv_find_pci_capability(struct pci_dev *pci_dev, NvU8 capability)
{
    u16 status = 0;
    u8  cap_ptr = 0, cap_id = 0xff;

    pci_read_config_word(pci_dev, PCI_STATUS, &status);
    status &= PCI_STATUS_CAP_LIST;
    if (!status)
        return 0;

    switch (pci_dev->hdr_type) {
        case PCI_HEADER_TYPE_NORMAL:
        case PCI_HEADER_TYPE_BRIDGE:
            pci_read_config_byte(pci_dev, PCI_CAPABILITY_LIST, &cap_ptr);
            break;
        default:
            return 0;
    }

    do {
        cap_ptr &= 0xfc;
        pci_read_config_byte(pci_dev, cap_ptr + PCI_CAP_LIST_ID, &cap_id);
        if (cap_id == capability)
            return cap_ptr;
        pci_read_config_byte(pci_dev, cap_ptr + PCI_CAP_LIST_NEXT, &cap_ptr);
    } while (cap_ptr && cap_id != 0xff);

    return 0;
}

/* make sure the pci_driver called probe for all of our devices.
 * we've seen cases where rivafb claims the device first and our driver
 * doesn't get called.
 */
int
nv_pci_count_devices(nvidia_stack_t *sp)
{
    struct pci_dev *pci_dev;
    int count = 0;

    if (NVreg_RegisterPCIDriver == 0)
    {
        return 0;
    }

    pci_dev = NV_PCI_GET_CLASS(PCI_CLASS_DISPLAY_VGA << 8, NULL);
    while (pci_dev)
    {
        if ((pci_dev->vendor == 0x10de) && (pci_dev->device >= 0x20) &&
            !rm_is_legacy_device(sp, pci_dev->device, pci_dev->subsystem_vendor,
                                 pci_dev->subsystem_device, TRUE))
        {
            count++;
            nv_pci_validate_assigned_gpus(pci_dev);
        }
        pci_dev = NV_PCI_GET_CLASS(PCI_CLASS_DISPLAY_VGA << 8, pci_dev);
    }

    pci_dev = NV_PCI_GET_CLASS(PCI_CLASS_DISPLAY_3D << 8, NULL);
    while (pci_dev)
    {
        if ((pci_dev->vendor == 0x10de) && (pci_dev->device >= 0x20) &&
            !rm_is_legacy_device(sp, pci_dev->device, pci_dev->subsystem_vendor,
                                 pci_dev->subsystem_device, TRUE))
        {
            count++;
            nv_pci_validate_assigned_gpus(pci_dev);
        }
        pci_dev = NV_PCI_GET_CLASS(PCI_CLASS_DISPLAY_3D << 8, pci_dev);
    }

    if (NV_IS_ASSIGN_GPU_PCI_INFO_SPECIFIED())
    {
        NvU32 i;

        for (i = 0; i < nv_assign_gpu_count; i++)
        {
            if (nv_assign_gpu_pci_info[i].valid == NV_TRUE)
                count++;
        }
    }

    return count;
}

#if defined(NV_PCI_ERROR_RECOVERY)
static pci_ers_result_t
nv_pci_error_detected(
    struct pci_dev *pci_dev,
    enum pci_channel_state error
)
{
    nv_linux_state_t *nvl = pci_get_drvdata(pci_dev);

    if ((nvl == NULL) || (nvl->pci_dev != pci_dev))
    {
        nv_printf(NV_DBG_ERRORS, "NVRM: %s: invalid device!\n", __FUNCTION__);
        return PCI_ERS_RESULT_NONE;
    }

    /*
     * Tell Linux to continue recovery of the device. The kernel will enable
     * MMIO for the GPU and call the mmio_enabled callback.
     */
    return PCI_ERS_RESULT_CAN_RECOVER;
}

static pci_ers_result_t
nv_pci_mmio_enabled(
    struct pci_dev *pci_dev
)
{
    NV_STATUS         status = NV_OK;
    nv_stack_t       *sp = NULL;
    nv_linux_state_t *nvl = pci_get_drvdata(pci_dev);
    nv_state_t       *nv = NULL;

    if ((nvl == NULL) || (nvl->pci_dev != pci_dev))
    {
        nv_printf(NV_DBG_ERRORS, "NVRM: %s: invalid device!\n", __FUNCTION__);
        goto done;
    }

    nv = NV_STATE_PTR(nvl);

    if (nv_kmem_cache_alloc_stack(&sp) != 0)
    {
        nv_printf(NV_DBG_ERRORS, "NVRM: %s: failed to allocate stack!\n",
            __FUNCTION__);
        goto done;
    }

    NV_DEV_PRINTF(NV_DBG_ERRORS, nv, "A fatal error was detected.\n");

    /*
     * MMIO should be re-enabled now. If we still get bad reads, there's
     * likely something wrong with the adapter itself that will require a
     * reset. This should let us know whether the GPU has completely fallen
     * off the bus or just did something the host didn't like.
     */
    status = rm_is_supported_device(sp, nv);
    if (status != NV_OK)
    {
        NV_DEV_PRINTF(NV_DBG_ERRORS, nv,
            "The kernel has enabled MMIO for the device,\n"
            "NVRM: but it still appears unreachable. The device\n"
            "NVRM: will not function properly until it is reset.\n");
    }

    status = rm_log_gpu_crash(sp, nv);
    if (status != NV_OK)
    {
        NV_DEV_PRINTF_STATUS(NV_DBG_ERRORS, nv, status,
                      "Failed to log crash data\n");
        goto done;
    }

done:
    if (sp != NULL)
    {
        nv_kmem_cache_free_stack(sp);
    }

    /*
     * Tell Linux to abandon recovery of the device. The kernel might be able
     * to recover the device, but RM and clients don't yet support that.
     */
    return PCI_ERS_RESULT_DISCONNECT;
}

struct pci_error_handlers nv_pci_error_handlers = {
    .error_detected = nv_pci_error_detected,
    .mmio_enabled   = nv_pci_mmio_enabled,
};
#endif

#if defined(CONFIG_PM)
extern struct dev_pm_ops nv_pm_ops;
#endif

struct pci_driver nv_pci_driver = {
    .name      = MODULE_NAME,
    .id_table  = nv_pci_table,
    .probe     = nv_pci_probe,
    .remove    = nv_pci_remove,
#if defined(CONFIG_PM)
    .driver.pm = &nv_pm_ops,
#endif
#if defined(NV_PCI_ERROR_RECOVERY)
    .err_handler = &nv_pci_error_handlers,
#endif
};

void nv_pci_unregister_driver(void)
{
    if (NVreg_RegisterPCIDriver == 0)
    {
        return;
    }
    return pci_unregister_driver(&nv_pci_driver);
}

int nv_pci_register_driver(void)
{
    if (NVreg_RegisterPCIDriver == 0)
    {
        return 0;
    }
    return pci_register_driver(&nv_pci_driver);
}
