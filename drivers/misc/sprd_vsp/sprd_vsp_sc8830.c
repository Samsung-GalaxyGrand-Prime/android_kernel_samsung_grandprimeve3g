/*
 * Copyright (C) 2012 Spreadtrum Communications Inc.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */


#include <linux/platform_device.h>
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/wait.h>
#include <linux/io.h>
#include <linux/uaccess.h>
#include <linux/clk.h>
#include <linux/debugfs.h>
#include <linux/interrupt.h>
#include <linux/cdev.h>
#include <linux/mm.h>
#include <linux/miscdevice.h>
#include <linux/sched.h>
#include <linux/clk.h>
#ifdef CONFIG_OF
#include <linux/clk-provider.h>
#endif
#include <linux/semaphore.h>
#include <linux/slab.h>
#include <linux/wakelock.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>

#include <video/sprd_vsp.h>

#include <soc/sprd/sci.h>
#include <soc/sprd/sci_glb_regs.h>

#include <linux/sprd_iommu.h>

#define VSP_MINOR MISC_DYNAMIC_MINOR
#define VSP_AQUIRE_TIMEOUT_MS 500
#define VSP_INIT_TIMEOUT_MS 200

#define USE_INTERRUPT
/*#define RT_VSP_THREAD*/

#define DEFAULT_FREQ_DIV 0x0

#define ARM_ACCESS_CTRL_OFF         0x0
#define ARM_ACCESS_STATUS_OFF   0x04
#define MCU_CTRL_SET_OFF                0x08
#define ARM_INT_STS_OFF                     0x10        //from OPENRISC
#define ARM_INT_MASK_OFF                0x14
#define ARM_INT_CLR_OFF                     0x18
#define ARM_INT_RAW_OFF                 0x1C
#define WB_ADDR_SET0_OFF                0x20
#define WB_ADDR_SET1_OFF                0x24

static unsigned long SPRD_VSP_PHYS = 0;
static unsigned long SPRD_VSP_BASE = 0;
static unsigned long VSP_GLB_REG_BASE = 0;
#ifdef CONFIG_MACH_GRANDPRIMEVE3G
static unsigned long sprd_vsp_range_size = 0;
#endif
#define VSP_INT_STS_OFF            0x0             //from VSP
#define VSP_INT_MASK_OFF        0x04
#define VSP_INT_CLR_OFF           0x08
#define VSP_INT_RAW_OFF         0x0c

struct vsp_fh {
    int is_vsp_aquired;
    int is_clock_enabled;

    wait_queue_head_t wait_queue_work;
    int condition_work;
    int vsp_int_status;
};

struct vsp_dev {
    unsigned int freq_div;

    struct semaphore vsp_mutex;

    struct clk *vsp_clk;
    struct clk *vsp_parent_clk;
    struct clk *mm_clk;
    struct clk *mm_clk_axi;

    unsigned int irq;
    unsigned int version;

    struct vsp_fh *vsp_fp;
    struct device_node *dev_np;
    bool  light_sleep_en;
};

static struct vsp_dev vsp_hw_dev;
static struct wake_lock vsp_wakelock;
static atomic_t vsp_instance_cnt = ATOMIC_INIT(0);

struct clock_name_map_t {
    unsigned long freq;
    char *name;
};

#ifdef CONFIG_OF
static struct clock_name_map_t clock_name_map[4];
#else
static struct clock_name_map_t clock_name_map[] = {
#if defined(CONFIG_ARCH_SCX35LT8)
    {307200000,"clk_307m2"},
    {256000000,"clk_256m"},
    {128000000,"clk_128m"},
    {96000000,"clk_96m"}
#elif defined(CONFIG_ARCH_SCX35L)
    {312000000,"clk_312m"},
    {256000000,"clk_256m"},
    {128000000,"clk_128m"},
    {76800000,"clk_76m8"}
#elif defined(CONFIG_ARCH_SCX15)
    {192000000,"clk_192m"},
    {153600000,"clk_153m6"},
    {128000000,"clk_128m"},
    {76800000,"clk_76m8"}
#elif defined(CONFIG_ARCH_SCX20)
    {192000000,"clk_192m"},
    {128000000,"clk_128m"},
    {76800000,"clk_76m8"}
#else
    {256000000,"clk_256m"},
    {192000000,"clk_192m"},
    {128000000,"clk_128m"},
    {76800000,"clk_76m8"}
#endif
};
#endif

static int max_freq_level = ARRAY_SIZE(clock_name_map);

static char *vsp_get_clk_src_name(unsigned int freq_level)
{
    if (freq_level >= max_freq_level ) {
        printk(KERN_INFO "set freq_level to 0");
        freq_level = 0;
    }

    return clock_name_map[freq_level].name;
}

static int find_vsp_freq_level(unsigned long freq)
{
    int level = 0;
    int i;
    for (i = 0; i < max_freq_level; i++) {
        if (clock_name_map[i].freq == freq) {
            level = i;
            break;
        }
    }
    return level;
}

#if defined(CONFIG_ARCH_SCX35)
#ifdef USE_INTERRUPT
static irqreturn_t vsp_isr(int irq, void *data);
#endif
#endif
static long vsp_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    int ret;
    struct clk *clk_parent;
    char *name_parent;
    unsigned long frequency;
    struct vsp_fh *vsp_fp = filp->private_data;

    if (vsp_fp == NULL) {
        printk(KERN_ERR "vsp_ioctl error occured, vsp_fp == NULL\n");
        return  -EINVAL;
    }

    switch (cmd) {
    case VSP_CONFIG_FREQ:
        get_user(vsp_hw_dev.freq_div, (int __user *)arg);
        name_parent = vsp_get_clk_src_name(vsp_hw_dev.freq_div);
        clk_parent = clk_get(NULL, name_parent);
        if ((!clk_parent )|| IS_ERR(clk_parent)) {
            printk(KERN_ERR "clock[%s]: failed to get parent [%s] by clk_get()!\n", "clk_vsp", name_parent);
            return -EINVAL;
        }
        ret = clk_set_parent(vsp_hw_dev.vsp_clk, clk_parent);
        if (ret) {
            printk(KERN_ERR "clock[%s]: clk_set_parent() failed!","clk_vsp");
            return -EINVAL;
        } else {
            clk_put(vsp_hw_dev.vsp_parent_clk);
            vsp_hw_dev.vsp_parent_clk = clk_parent;
        }
        pr_debug(KERN_INFO "VSP_CONFIG_FREQ %d\n", vsp_hw_dev.freq_div);
        break;
    case VSP_GET_FREQ:
        frequency = clk_get_rate(vsp_hw_dev.vsp_clk);
        ret = find_vsp_freq_level(frequency);
        put_user(ret, (int __user *)arg);
        pr_debug(KERN_INFO "vsp ioctl VSP_GET_FREQ %d\n", ret);
        break;
    case VSP_ENABLE:
        pr_debug("vsp ioctl VSP_ENABLE\n");
        wake_lock(&vsp_wakelock);
        ret = clk_prepare_enable(vsp_hw_dev.vsp_clk);
        if (ret) {
            printk(KERN_ERR "###:vsp_hw_dev.vsp_clk: clk_enable() failed!\n");
            return ret;
        } else {
            pr_debug("###vsp_hw_dev.vsp_clk: clk_enable() ok.\n");
        }
#ifdef CONFIG_OF
        sci_glb_set(SPRD_MMAHB_BASE+0x08, BIT(5));
#endif
        vsp_fp->is_clock_enabled= 1;
        break;
    case VSP_DISABLE:
        pr_debug("vsp ioctl VSP_DISABLE\n");
        clk_disable_unprepare(vsp_hw_dev.vsp_clk);
#ifdef CONFIG_OF
        sci_glb_clr(SPRD_MMAHB_BASE+0x08, BIT(5));
#endif
        vsp_fp->is_clock_enabled = 0;
        wake_unlock(&vsp_wakelock);
        break;
    case VSP_ACQUAIRE:
        pr_debug("vsp ioctl VSP_ACQUAIRE begin\n");
        ret = down_timeout(&vsp_hw_dev.vsp_mutex,
                           msecs_to_jiffies(VSP_AQUIRE_TIMEOUT_MS));
        if (ret) {
            printk(KERN_ERR "vsp error timeout\n");
            //up(&vsp_hw_dev.vsp_mutex);
            return ret;
        }
#ifdef RT_VSP_THREAD
        if (!rt_task(current)) {
            struct sched_param schedpar;
            int ret;
            struct cred *new = prepare_creds();
            cap_raise(new->cap_effective, CAP_SYS_NICE);
            commit_creds(new);
            schedpar.sched_priority = 1;
            ret = sched_setscheduler(current, SCHED_RR, &schedpar);
            if (ret!=0)
                printk(KERN_ERR "vsp change pri fail a\n");
        }
#endif
        vsp_fp->is_vsp_aquired = 1;
        vsp_hw_dev.vsp_fp = vsp_fp;

        if (vsp_hw_dev.light_sleep_en) {
            pr_debug("VSP mmi_clk open\n");
            ret = clk_prepare_enable(vsp_hw_dev.mm_clk);
            if (ret) {
                printk(KERN_ERR "###:vsp_hw_dev.mm_clk: clk_prepare_enable() failed!\n");
                return ret;
            } else {
                pr_debug("###vsp_hw_dev.mm_clk: clk_prepare_enable() ok.\n");
            }
#if defined(CONFIG_SPRD_IOMMU)
            sprd_iommu_module_enable(IOMMU_MM);
#endif
        }
        pr_debug("vsp ioctl VSP_ACQUAIRE end\n");
        break;
    case VSP_RELEASE:
        pr_debug("vsp ioctl VSP_RELEASE\n");

        if (vsp_hw_dev.light_sleep_en) {
#if defined(CONFIG_SPRD_IOMMU)
            sprd_iommu_module_disable(IOMMU_MM);
#endif
            clk_disable_unprepare(vsp_hw_dev.mm_clk);
            pr_debug("VSP mmi_clk close\n");
        }
        vsp_fp->is_vsp_aquired = 0;
        vsp_hw_dev.vsp_fp = NULL;
        up(&vsp_hw_dev.vsp_mutex);
        break;
#ifdef USE_INTERRUPT
    case VSP_COMPLETE:
        pr_debug("vsp ioctl VSP_COMPLETE\n");
        ret = wait_event_interruptible_timeout(
                  vsp_fp->wait_queue_work,
                  vsp_fp->condition_work,
                  msecs_to_jiffies(VSP_INIT_TIMEOUT_MS));
        if (ret == -ERESTARTSYS) {
            printk(KERN_INFO "vsp complete -ERESTARTSYS\n");
            vsp_fp->vsp_int_status |= (1<<30);
            put_user(vsp_fp->vsp_int_status, (int __user *)arg);
            ret = -EINVAL;
        } else
        {
            vsp_fp->vsp_int_status &= (~ (1<<30));
            if (ret == 0) {
                printk(KERN_ERR "vsp complete  timeout\n");
                vsp_fp->vsp_int_status |= (1<<31);
                ret = -ETIMEDOUT;
                /*clear vsp int*/
                __raw_writel((1<<1) |(1<<2)|(1<<4)|(1<<5), (void *)(VSP_GLB_REG_BASE+VSP_INT_CLR_OFF));
                __raw_writel((1<<0)|(1<<1)|(1<<2), (void *)(SPRD_VSP_BASE+ARM_INT_CLR_OFF));
            } else {
                ret = 0;
            }
            put_user(vsp_fp->vsp_int_status, (int __user *)arg);
            vsp_fp->vsp_int_status = 0;
            vsp_fp->condition_work = 0;
        }
        pr_debug("vsp ioctl VSP_COMPLETE end\n");
        return ret;
        break;
#endif
    case VSP_RESET:
        pr_debug("vsp ioctl VSP_RESET\n");
        sci_glb_set(SPRD_MMAHB_BASE+0x04, BIT(4));
        sci_glb_clr(SPRD_MMAHB_BASE+0x04, BIT(4));
        break;
    case VSP_HW_INFO:
    {
        u32 mm_eb_reg;

        pr_debug("vsp ioctl VSP_HW_INFO\n");
        mm_eb_reg = sci_glb_read(SPRD_AONAPB_BASE, 0xFFFFFFFF);
        put_user(mm_eb_reg, (int __user *)arg);
    }
    break;

    case VSP_VERSION:
    {
        printk(KERN_INFO "vsp version -enter\n");
        put_user(vsp_hw_dev.version, (int __user *)arg);
    }
    break;

    default:
        return -EINVAL;
    }
    return 0;
}

#ifdef USE_INTERRUPT
static irqreturn_t vsp_isr(int irq, void *data)
{
    int int_status;
    int ret = 0xff; // 0xff : invalid
    struct vsp_fh *vsp_fp = vsp_hw_dev.vsp_fp;

    if (vsp_fp == NULL) {
        //printk(KERN_ERR "vsp_isr error occured, vsp_fp == NULL\n");
        return  IRQ_NONE;
    }

    //check which module occur interrupt and clear coresponding bit
    int_status =  __raw_readl((void *)(VSP_GLB_REG_BASE+VSP_INT_STS_OFF));
    if((int_status >> 1) & 0x1) // VLC SLICE DONE
    {
        __raw_writel((1<<1), (void *)(VSP_GLB_REG_BASE+VSP_INT_CLR_OFF));
        ret = (1<<1);
    } else if((int_status >> 2) & 0x1) // MBW SLICE DONE
    {
        __raw_writel((1<<2), (void *)(VSP_GLB_REG_BASE+VSP_INT_CLR_OFF));
        ret = (1<<2);
    } else if((int_status >> 4) & 0x1) // VLD ERR
    {
        __raw_writel((1<<4), (void *)(VSP_GLB_REG_BASE+VSP_INT_CLR_OFF));
        ret = (1<<4);
    } else if((int_status >> 5) & 0x1) // TIMEOUT ERR
    {
        __raw_writel((1<<5), (void *)(VSP_GLB_REG_BASE+VSP_INT_CLR_OFF));
        ret = (1<<5);
    }else
    {
        return IRQ_NONE;
    }

    //clear VSP accelerator interrupt bit
    int_status =  __raw_readl((void *)(SPRD_VSP_BASE+ARM_INT_STS_OFF));
    if ((int_status >> 2) & 0x1) //VSP ACC INT
    {
        __raw_writel((1<<2), (void *)(SPRD_VSP_BASE+ARM_INT_CLR_OFF));
    }

    vsp_fp->vsp_int_status = ret;
    vsp_fp->condition_work = 1;
    wake_up_interruptible(&vsp_fp->wait_queue_work);

    return IRQ_HANDLED;
}
#endif

#ifdef CONFIG_OF
static const struct of_device_id  of_match_table_vsp[] = {
    { .compatible = "sprd,sprd_vsp", },
    { },
};

static int vsp_parse_dt(struct device *dev)
{
    struct device_node *np = dev->of_node;
    struct resource res;
    u32 clock_parent_info[2];
    u32 i;
    int ret;

    printk(KERN_INFO "vsp_parse_dt called !\n");

    ret = of_address_to_resource(np, 0, &res);
    if(ret < 0) {
        dev_err(dev, "no reg of property specified\n");
        printk(KERN_ERR "vsp: failed to parse_dt!\n");
        return -EINVAL;
    }

    SPRD_VSP_PHYS = res.start;
    SPRD_VSP_BASE = (unsigned long)ioremap_nocache(res.start,
                    resource_size(&res));
    if(!SPRD_VSP_BASE)
        BUG();

    VSP_GLB_REG_BASE = SPRD_VSP_BASE + 0x1000;
#ifdef CONFIG_MACH_GRANDPRIMEVE3G
    sprd_vsp_range_size = resource_size(&res);
#endif

    printk(KERN_INFO "sprd_vsp_phys = %lx\n", SPRD_VSP_PHYS);
    printk(KERN_INFO "sprd_vsp_base = %lx\n", SPRD_VSP_BASE);
    printk(KERN_INFO "vsp_glb_reg_base = %lx\n", VSP_GLB_REG_BASE);

    ret = of_property_read_u32(np, "version", &(vsp_hw_dev.version));
    if(0 != ret) {
        printk(KERN_ERR "vsp: read version fail (%d)\n", ret);
        return -EINVAL;
    }

    vsp_hw_dev.irq = irq_of_parse_and_map(np, 0);
    vsp_hw_dev.dev_np = np;

    printk(KERN_INFO "vsp: irq = 0x%x, version = 0x%0x\n", vsp_hw_dev.irq, vsp_hw_dev.version);

    ret = of_property_read_u32_array(np, "clock-parent-info", clock_parent_info, 2);
    if(0 != ret) {
        printk(KERN_ERR "vsp: read clock-parent-info fail (%d)\n", ret);
        return -EINVAL;
    }
    printk(KERN_INFO "vsp: clock-parent-info [%d, %d]\n", clock_parent_info[0], clock_parent_info[1]);

    max_freq_level = clock_parent_info[1];
    if (max_freq_level > 4) {
        printk(KERN_ERR "vsp: max_freq_level is invalid\n");
        return -EINVAL;
    }

    for (i = 0; i < max_freq_level; i++) {
        struct clk *clk_parent;
        char *name_parent;
        unsigned long frequency;

        name_parent = of_clk_get_parent_name(np,  i+clock_parent_info[0]);
        clk_parent = clk_get(NULL, name_parent);
        frequency = clk_get_rate(clk_parent);
        printk(KERN_INFO "vsp: clock_name_map[%d] = (%d, %s)\n", frequency, name_parent);

        clock_name_map[i].name = name_parent;
        clock_name_map[i].freq = frequency;
    }

    return 0;
}
#else
static int  vsp_parse_dt(
    struct device *dev)
{
    vsp_hw_dev.irq = IRQ_VSP_INT;
    vsp_hw_dev.version = 0;
    return 0;
}
#endif

static int vsp_nocache_mmap(struct file *filp, struct vm_area_struct *vma)
{
    printk(KERN_INFO "@vsp[%s]\n", __FUNCTION__);
    vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
    vma->vm_pgoff     = (SPRD_VSP_PHYS>>PAGE_SHIFT);

#ifdef CONFIG_MACH_GRANDPRIMEVE3G
    if ((vma->vm_end - vma->vm_start) > sprd_vsp_range_size )
	    return -EAGAIN;
#endif

    if (remap_pfn_range(vma,vma->vm_start, vma->vm_pgoff,
                        vma->vm_end - vma->vm_start, vma->vm_page_prot))
        return -EAGAIN;
    printk(KERN_INFO "@vsp mmap %x,%lx,%x\n", (unsigned int)PAGE_SHIFT,
           (unsigned long)vma->vm_start,
           (unsigned int)(vma->vm_end - vma->vm_start));
    return 0;
}

static int vsp_set_mm_clk(void)
{
    int ret =0;
    struct clk *clk_mm_axi;
    struct clk *clk_mm_i;
    struct clk *clk_vsp;
    struct clk *clk_parent;
    char *name_parent;
    int instance_cnt = atomic_read(&vsp_instance_cnt);

    printk(KERN_INFO "vsp_set_mm_clk: vsp_instance_cnt %d\n", instance_cnt);

#if defined(CONFIG_ARCH_SCX35)

#ifdef CONFIG_OF
    clk_mm_axi = of_clk_get_by_name(vsp_hw_dev.dev_np, "clk_mm_axi");
    if (IS_ERR(clk_mm_axi) || (!clk_mm_axi)) {
        printk(KERN_ERR "###: Failed : Can't get clock [%s}!\n",
               "clk_mm_axi");
        printk(KERN_ERR "###: clk_mm_axi =  %p\n", clk_mm_axi);
        vsp_hw_dev.mm_clk_axi = NULL;
        vsp_hw_dev.light_sleep_en = false;
    } else {
        vsp_hw_dev.mm_clk_axi= clk_mm_axi;
        vsp_hw_dev.light_sleep_en = true;
    }
#endif

#ifdef CONFIG_OF
    clk_mm_i = of_clk_get_by_name(vsp_hw_dev.dev_np, "clk_mm_i");
#else
    clk_mm_i = clk_get(NULL, "clk_mm_i");
#endif
    if (IS_ERR(clk_mm_i) || (!clk_mm_i)) {
        printk(KERN_ERR "###: Failed : Can't get clock [%s}!\n",
               "clk_mm_i");
        printk(KERN_ERR "###: clk_mm_i =  %p\n", clk_mm_i);
        ret = -EINVAL;
        goto errout;
    } else {
        vsp_hw_dev.mm_clk= clk_mm_i;
    }
#endif

    if (vsp_hw_dev.mm_clk_axi) {
        printk(KERN_INFO "VSP mmi_clk_axi open");
        ret = clk_prepare(vsp_hw_dev.mm_clk_axi);
        if (ret) {
            printk(KERN_ERR "###:vsp_hw_dev.mm_clk_axi: clk_prepare() failed!\n");
            goto errout;
        } else {
            pr_debug("###vsp_hw_dev.mm_clk_axi: clk_prepare() ok.\n");
        }
    }

    printk(KERN_INFO "VSP mmi_clk open\n");
    ret = clk_prepare_enable(vsp_hw_dev.mm_clk);
    if (ret) {
        printk(KERN_ERR "###:vsp_hw_dev.mm_clk: clk_prepare_enable() failed!\n");
        goto errout;
    } else {
        pr_debug("###vsp_hw_dev.mm_clk: clk_prepare_enable() ok.\n");
    }

#ifdef CONFIG_OF
    clk_vsp = of_clk_get_by_name(vsp_hw_dev.dev_np, "clk_vsp");
#else
    clk_vsp = clk_get(NULL, "clk_vsp");
#endif
    if (IS_ERR(clk_vsp) || (!clk_vsp)) {
        printk(KERN_ERR "###: Failed : Can't get clock [%s}!\n",
               "clk_vsp");
        printk(KERN_ERR "###: vsp_clk =  %p\n", clk_vsp);
        ret = -EINVAL;
        goto errout;
    } else {
        vsp_hw_dev.vsp_clk = clk_vsp;
    }

    name_parent = vsp_get_clk_src_name(vsp_hw_dev.freq_div);
    clk_parent = clk_get(NULL, name_parent);
    if ((!clk_parent )|| IS_ERR(clk_parent) ) {
        printk(KERN_ERR "clock[%s]: failed to get parent in probe[%s] \
by clk_get()!\n", "clk_vsp", name_parent);
        ret = -EINVAL;
        goto errout;
    } else {
        vsp_hw_dev.vsp_parent_clk = clk_parent;
    }

    ret = clk_set_parent(vsp_hw_dev.vsp_clk, vsp_hw_dev.vsp_parent_clk);
    if (ret) {
        printk(KERN_ERR "clock[%s]: clk_set_parent() failed in probe!",
               "clk_vsp");
        ret = -EINVAL;
        goto errout;
    }

    printk("vsp parent clock name %s\n", name_parent);
    printk("vsp_freq %d Hz\n",
           (int)clk_get_rate(vsp_hw_dev.vsp_clk));

    if (vsp_hw_dev.light_sleep_en) {
        clk_disable_unprepare(vsp_hw_dev.mm_clk);
        pr_debug("VSP mmi_clk close\n");
    }

    return 0;
errout:
#if defined(CONFIG_ARCH_SCX35)
    if (vsp_hw_dev.mm_clk_axi) {
        clk_put(vsp_hw_dev.mm_clk_axi);
    }

    if (vsp_hw_dev.mm_clk) {
        clk_put(vsp_hw_dev.mm_clk);
    }
#endif

    if (vsp_hw_dev.vsp_clk) {
        clk_put(vsp_hw_dev.vsp_clk);
    }

    if (vsp_hw_dev.vsp_parent_clk) {
        clk_put(vsp_hw_dev.vsp_parent_clk);
    }
    return ret;
}

static int vsp_open(struct inode *inode, struct file *filp)
{
    int ret;
    struct vsp_fh *vsp_fp = kmalloc(sizeof(struct vsp_fh), GFP_KERNEL);

    printk(KERN_INFO "vsp_open called %p\n", vsp_fp);

    if (vsp_fp == NULL) {
        printk(KERN_ERR "vsp open error occured\n");
        return  -EINVAL;
    }
    filp->private_data = vsp_fp;
    vsp_fp->is_clock_enabled = 0;
    vsp_fp->is_vsp_aquired = 0;

    init_waitqueue_head(&vsp_fp->wait_queue_work);
    vsp_fp->vsp_int_status = 0;
    vsp_fp->condition_work = 0;

    ret = vsp_set_mm_clk();

    atomic_inc_return(&vsp_instance_cnt);

    printk(KERN_INFO "vsp_open: ret %d\n", ret);

    return ret;
}

static int vsp_release (struct inode *inode, struct file *filp)
{
    struct vsp_fh *vsp_fp = filp->private_data;
    int instance_cnt = atomic_read(&vsp_instance_cnt);

    if (vsp_fp == NULL) {
        printk(KERN_ERR "vsp_release error occured, vsp_fp == NULL\n");
        return  -EINVAL;
    }

    printk(KERN_INFO "vsp_release: instance_cnt %d\n", instance_cnt);

    atomic_dec_return(&vsp_instance_cnt);

    if (vsp_fp->is_clock_enabled) {
        printk(KERN_ERR "error occured and close clock \n");
        clk_disable_unprepare(vsp_hw_dev.vsp_clk);
        vsp_fp->is_clock_enabled = 0;
    }

    if (vsp_fp->is_vsp_aquired) {
        printk(KERN_ERR "error occured and up vsp_mutex \n");
        up(&vsp_hw_dev.vsp_mutex);
    }

    printk(KERN_INFO "vsp_release %p\n", vsp_fp);
    kfree(filp->private_data);
    filp->private_data=NULL;

    if (!vsp_hw_dev.light_sleep_en) {
        clk_disable_unprepare(vsp_hw_dev.mm_clk);
        printk(KERN_INFO "VSP mmi_clk close!\n");
    }

    if (vsp_hw_dev.mm_clk_axi) {
        clk_unprepare(vsp_hw_dev.mm_clk_axi);
        printk(KERN_INFO "VSP mm_clk_axi close!\n");
    }

    return 0;
}

static const struct file_operations vsp_fops =
{
    .owner = THIS_MODULE,
    .unlocked_ioctl = vsp_ioctl,
    .mmap  = vsp_nocache_mmap,
    .open = vsp_open,
    .release = vsp_release,
#ifdef CONFIG_COMPAT
    .compat_ioctl   = vsp_ioctl,
#endif
};

static struct miscdevice vsp_dev = {
    .minor   = VSP_MINOR,
    .name   = "sprd_vsp",
    .fops   = &vsp_fops,
};

static int vsp_suspend(struct platform_device *pdev, pm_message_t state)
{
    int ret=-1;
    int cnt;
    int instance_cnt = atomic_read(&vsp_instance_cnt);

    for (cnt = 0; cnt < instance_cnt; cnt++) {
        if (!vsp_hw_dev.light_sleep_en) {
            clk_disable_unprepare(vsp_hw_dev.mm_clk);
            pr_debug("VSP mm_clk close\n");
        } else {
            clk_unprepare(vsp_hw_dev.mm_clk_axi);
            pr_debug("VSP mm_clk_axi close\n");
        }

        printk(KERN_INFO "vsp_suspend, cnt: %d\n", cnt);
    }

    return 0;
}

static int vsp_resume(struct platform_device *pdev)
{
    int ret = 0;
    int cnt;
    int instance_cnt = atomic_read(&vsp_instance_cnt);

    for (cnt = 0; cnt < instance_cnt; cnt++) {
        ret = vsp_set_mm_clk();
        printk(KERN_INFO "vsp_resume, cnt: %d\n", cnt);
    }

    return ret;
}

static int vsp_probe(struct platform_device *pdev)
{
    int ret;

    printk(KERN_INFO "vsp_probe called !\n");

#ifdef CONFIG_OF
    if (pdev->dev.of_node) {
        ret = vsp_parse_dt(&pdev->dev);
    }
#else
    ret = vsp_parse_dt(&pdev->dev);
#endif

    wake_lock_init(&vsp_wakelock, WAKE_LOCK_SUSPEND,
                   "pm_message_wakelock_vsp");

    sema_init(&vsp_hw_dev.vsp_mutex, 1);

    vsp_hw_dev.freq_div = DEFAULT_FREQ_DIV;

    vsp_hw_dev.vsp_clk = NULL;
    vsp_hw_dev.vsp_parent_clk = NULL;
    vsp_hw_dev.mm_clk= NULL;
    vsp_hw_dev.mm_clk_axi = NULL;
    vsp_hw_dev.vsp_fp = NULL;
    vsp_hw_dev.light_sleep_en = false;

    ret = misc_register(&vsp_dev);
    if (ret) {
        printk(KERN_ERR "cannot register miscdev on minor=%d (%d)\n",
               VSP_MINOR, ret);
        goto errout;
    }

#ifdef USE_INTERRUPT
    /* register isr */
    ret = request_irq(vsp_hw_dev.irq, vsp_isr, IRQF_DISABLED|IRQF_SHARED, "VSP", &vsp_hw_dev);
    if (ret) {
        printk(KERN_ERR "vsp: failed to request irq!\n");
        ret = -EINVAL;
        goto errout;
    }
#endif

    return 0;

errout:
    misc_deregister(&vsp_dev);

    return ret;
}

static int vsp_remove(struct platform_device *pdev)
{
    printk(KERN_INFO "vsp_remove called !\n");

    misc_deregister(&vsp_dev);

#ifdef USE_INTERRUPT
    free_irq(vsp_hw_dev.irq, &vsp_hw_dev);
#endif

    if (vsp_hw_dev.vsp_clk) {
        clk_put(vsp_hw_dev.vsp_clk);
    }

    if (vsp_hw_dev.vsp_parent_clk) {
        clk_put(vsp_hw_dev.vsp_parent_clk);
    }

    printk(KERN_INFO "vsp_remove Success !\n");
    return 0;
}

static struct platform_driver vsp_driver = {
    .probe    = vsp_probe,
    .remove   = vsp_remove,
#if !defined(CONFIG_ARCH_SCX35L)
    .suspend = vsp_suspend,
    .resume = vsp_resume,
#endif
    .driver   = {
        .owner = THIS_MODULE,
        .name = "sprd_vsp",
#ifdef CONFIG_OF
        .of_match_table = of_match_ptr(of_match_table_vsp) ,
#endif
    },
};

static int __init vsp_init(void)
{
    printk(KERN_INFO "vsp_init called !\n");
    if (platform_driver_register(&vsp_driver) != 0) {
        printk(KERN_ERR "platform device vsp drv register Failed \n");
        return -1;
    }
    return 0;
}

static void __exit vsp_exit(void)
{
    printk(KERN_INFO "vsp_exit called !\n");
    platform_driver_unregister(&vsp_driver);
}

module_init(vsp_init);
module_exit(vsp_exit);

MODULE_DESCRIPTION("SPRD VSP Driver");
MODULE_LICENSE("GPL");
