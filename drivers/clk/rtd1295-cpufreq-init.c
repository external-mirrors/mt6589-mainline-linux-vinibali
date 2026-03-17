// SPDX-License-Identifier: GPL-2.0
/*
 * RTD1295 CPU Frequency Initialization & Monitor
 * 
 * Sets CPU frequency to 1400 MHz at boot and monitors it every 10 seconds
 * Performs CPU-intensive benchmark to verify actual speed
 */

#include <linux/module.h>
#include <linux/io.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/delay.h>
#include <linux/workqueue.h>
#include <linux/kthread.h>
#include <linux/sched.h>
#include <linux/mm.h>
#include <linux/vmstat.h>
#include <linux/sysinfo.h>

#define CRT_BASE		0x98000000
#define SYS_GROUP1_CK_SEL	0x18   /* Clock source selector */
#define SYS_PLL_SCPU1		0x104  /* SCPU PLL power control */
#define SCPU_SSC_OFFSET		0x500
#define SCPU_PLL_OFFSET		0x504
#define SCPU_RDY_OFFSET		0x51C
#define SCPU_DIV_OFFSET		0x30

/* For 1400 MHz: N=48, F=1745 */
#define SCPU_1400MHZ_VAL	0x186D1
/* Divider register: bits 7-8, value 1 = div by 1, 2 = div by 2, 3 = div by 4 */
#define SCPU_DIV_SHIFT		7
#define SCPU_DIV_MASK		(0x3 << SCPU_DIV_SHIFT)  /* Bits 7-8 */
#define SCPU_DIV_VAL_1		(1 << SCPU_DIV_SHIFT)    /* Value 1 at bits 7-8 */

/* Monitor thread */
static struct task_struct *monitor_thread;
static bool monitor_running = true;
static bool disable_monitor = false;

static int __init rtd1295_disable_setup(char *str)
{
	disable_monitor = true;
	return 1;
}
__setup("rtd1295.disable", rtd1295_disable_setup);

/* CPU-intensive benchmark - calculate primes for 2 seconds */
static unsigned long cpu_benchmark(void)
{
	unsigned long count = 0;
	unsigned long i, j;
	bool is_prime;
	ktime_t start, end;
	s64 elapsed_ms;
	
	start = ktime_get();
	
	/* Find primes up to a large number */
	for (i = 2; count < 100000; i++) {
		is_prime = true;
		for (j = 2; j * j <= i; j++) {
			if (i % j == 0) {
				is_prime = false;
				break;
			}
		}
		if (is_prime)
			count++;
			
		/* Check if 2 seconds elapsed */
		if ((count & 0xFF) == 0) {  /* Check every 256 primes */
			end = ktime_get();
			elapsed_ms = ktime_to_ms(ktime_sub(end, start));
			if (elapsed_ms >= 2000)
				break;
		}
	}
	
	end = ktime_get();
	elapsed_ms = ktime_to_ms(ktime_sub(end, start));
	
	pr_info("RTD1295: CPU Benchmark - Found %lu primes in %llu ms (rate: %lu primes/sec)\n",
		count, (unsigned long long)elapsed_ms, 
		elapsed_ms > 0 ? (count * 1000) / (unsigned long)elapsed_ms : 0UL);
	
	return count;
}

/* Read and display system stats */
static void print_system_stats(void)
{
	struct sysinfo si;
	unsigned long free_pages, cached_pages;
	
	si_meminfo(&si);
	free_pages = global_zone_page_state(NR_FREE_PAGES);
	cached_pages = global_node_page_state(NR_FILE_PAGES);
	
	pr_info("RTD1295: Memory - Total: %lu MB, Free: %lu MB, Cached: %lu MB, Buffers: %lu MB\n",
		(si.totalram * si.mem_unit) >> 20,
		(free_pages << PAGE_SHIFT) >> 20,
		(cached_pages << PAGE_SHIFT) >> 20,
		(si.bufferram * si.mem_unit) >> 20);
	
	/* Load average is in fixed point format: divide by 2^11 (2048) */
	pr_info("RTD1295: Load average: %lu.%02lu %lu.%02lu %lu.%02lu\n",
		si.loads[0] >> 11, ((si.loads[0] & 0x7FF) * 100) >> 11,
		si.loads[1] >> 11, ((si.loads[1] & 0x7FF) * 100) >> 11,
		si.loads[2] >> 11, ((si.loads[2] & 0x7FF) * 100) >> 11);
}

/* Monitor thread - runs every 10 seconds */
static int monitor_thread_fn(void *data)
{
	void __iomem *crt_base;
	u32 ck_sel, div, pll, rdy;
	unsigned long benchmark_result;
	int cycle = 0;
	
	pr_info("RTD1295: Monitor thread started\n");
	
	while (!kthread_should_stop() && monitor_running) {
		/* Sleep for 8 seconds first */
		ssleep(8);
		
		if (kthread_should_stop() || !monitor_running)
			break;
		
		cycle++;
		pr_info("RTD1295: ========== Monitor Cycle %d (every 10s) ==========\n", cycle);
		
		/* Read clock registers */
		crt_base = ioremap(CRT_BASE, 0x1000);
		if (crt_base) {
			ck_sel = readl(crt_base + SYS_GROUP1_CK_SEL);
			div = readl(crt_base + SCPU_DIV_OFFSET);
			pll = readl(crt_base + SCPU_PLL_OFFSET);
			rdy = readl(crt_base + SCPU_RDY_OFFSET);
			
			pr_info("RTD1295: Clock Status:\n");
			pr_info("  CK_SEL=0x%08x (bit2=%s, using %s)\n", 
				ck_sel, (ck_sel & 0x4) ? "1" : "0",
				(ck_sel & 0x4) ? "OSC/27MHz" : "PLL/1400MHz");
			pr_info("  DIV=0x%08x (bits7-8=%d, divider=%s)\n", 
				div, (div >> SCPU_DIV_SHIFT) & 0x3,
				(((div >> SCPU_DIV_SHIFT) & 0x3) == 1) ? "1" : "NOT 1!");
			pr_info("  PLL=0x%08x (%s)\n", 
				pll, (pll == SCPU_1400MHZ_VAL) ? "1400MHz OK" : "WRONG!");
			pr_info("  RDY=0x%08x (bit20=%s, PLL %s)\n", 
				rdy, (rdy & BIT(20)) ? "1" : "0",
				(rdy & BIT(20)) ? "locked" : "NOT LOCKED");
			
			/* Check if we need to reprogram */
			if ((ck_sel & 0x4) || (((div >> SCPU_DIV_SHIFT) & 0x3) != 1) || 
			    (pll != SCPU_1400MHZ_VAL)) {
				pr_err("RTD1295: Clock registers CORRUPTED! Reprogramming...\n");
				
				/* Reprogram */
				writel(0x4, crt_base + SCPU_SSC_OFFSET);
				writel(SCPU_1400MHZ_VAL, crt_base + SCPU_PLL_OFFSET);
				writel(0xD, crt_base + SCPU_SSC_OFFSET);
				udelay(100);
				
				div = readl(crt_base + SCPU_DIV_OFFSET);
				div = (div & ~SCPU_DIV_MASK) | SCPU_DIV_VAL_1;
				writel(div, crt_base + SCPU_DIV_OFFSET);
				
				ck_sel = readl(crt_base + SYS_GROUP1_CK_SEL);
				ck_sel &= ~0x4;
				writel(ck_sel, crt_base + SYS_GROUP1_CK_SEL);
				
				pr_info("RTD1295: Reprogramming complete\n");
			}
			
			iounmap(crt_base);
		} else {
			pr_err("RTD1295: Failed to map CRT registers\n");
		}
		
		/* Print system stats */
		print_system_stats();
		
		/* Run CPU benchmark for 2 seconds */
		pr_info("RTD1295: Starting 2-second CPU benchmark...\n");
		benchmark_result = cpu_benchmark();
		
		pr_info("RTD1295: ========== End Monitor Cycle %d ==========\n\n", cycle);
	}
	
	pr_info("RTD1295: Monitor thread stopped\n");
	return 0;
}

static int __init rtd1295_cpufreq_init(void)
{
	void __iomem *crt_base;
	u32 val;
	int retry = 2000000;

	if (disable_monitor) {
		pr_info("RTD1295: Driver disabled via kernel parameter\n");
		return 0;
	}

	pr_info("RTD1295: Setting CPU frequency to 1400 MHz\n");

	crt_base = ioremap(CRT_BASE, 0x1000);
	if (!crt_base) {
		pr_err("RTD1295: Failed to map CRT registers\n");
		return -ENOMEM;
	}

	/* Step 0: Switch to OSC first (CRITICAL - vendor does this before PLL programming) */
	val = readl(crt_base + SYS_GROUP1_CK_SEL);
	pr_info("RTD1295: Initial clock selector = 0x%08x (bit 2 = %s)\n", 
		val, (val & 0x4) ? "OSC" : "PLL");
	
	if (!(val & 0x4)) {
		/* Currently using PLL - switch to OSC first */
		val |= 0x4;  /* Set bit 2 to select OSC */
		writel(val, crt_base + SYS_GROUP1_CK_SEL);
		val = readl(crt_base + SYS_GROUP1_CK_SEL);
		pr_info("RTD1295: Switched to OSC (27 MHz), clock selector = 0x%08x\n", val);
	}

	/* Step 1: Check and enable SCPU PLL power */
	val = readl(crt_base + SYS_PLL_SCPU1);
	pr_info("RTD1295: Initial PLL power = 0x%08x\n", val);
	if (val != 0x3) {
		writel(0x3, crt_base + SYS_PLL_SCPU1);
		udelay(200);  /* Vendor uses 200us delay */
		pr_info("RTD1295: Enabled PLL power\n");
	}

	/* Step 2: Clear oc_en (disable output clock) */
	val = readl(crt_base + SCPU_SSC_OFFSET);
	pr_info("RTD1295: Initial SSC register = 0x%08x\n", val);
	val = (val & ~0x7) | 0x4;  /* Set bits 0-2 to 0x4 */
	writel(val, crt_base + SCPU_SSC_OFFSET);
	val = readl(crt_base + SCPU_SSC_OFFSET);
	pr_info("RTD1295: After clearing oc_en, SSC = 0x%08x\n", val);

	/* Step 3: Write SCPU PLL value for 1400 MHz */
	writel(SCPU_1400MHZ_VAL, crt_base + SCPU_PLL_OFFSET);
	val = readl(crt_base + SCPU_PLL_OFFSET);
	pr_info("RTD1295: After writing PLL, PLL = 0x%08x (expected 0x%08x)\n", 
		val, SCPU_1400MHZ_VAL);

	/* Step 4: Set oc_en (enable output clock) */
	val = readl(crt_base + SCPU_SSC_OFFSET);
	val = (val & ~0x7) | 0x5;  /* Set bits 0-2 to 0x5 */
	writel(val, crt_base + SCPU_SSC_OFFSET);
	val = readl(crt_base + SCPU_SSC_OFFSET);
	pr_info("RTD1295: After setting oc_en, SSC = 0x%08x\n", val);

	/* Step 5: Wait for oc_done (bit 20 of RDY register) */
	while (--retry > 0) {
		val = readl(crt_base + SCPU_RDY_OFFSET);
		if (val & BIT(20)) {
			pr_info("RTD1295: PLL locked after %d iterations, RDY = 0x%08x\n", 
				2000000 - retry, val);
			break;
		}
	}
	
	if (retry <= 0) {
		val = readl(crt_base + SCPU_RDY_OFFSET);
		pr_err("RTD1295: Timeout waiting for PLL lock! RDY = 0x%08x (bit 20 = %s)\n",
			val, (val & BIT(20)) ? "SET" : "NOT SET");
	}

	/* Step 6: Set divider to 1 (no division) */
	val = readl(crt_base + SCPU_DIV_OFFSET);
	pr_info("RTD1295: Initial DIV register = 0x%08x (bits 7-8 = %d)\n", 
		val, (val >> SCPU_DIV_SHIFT) & 0x3);
	val = (val & ~SCPU_DIV_MASK) | SCPU_DIV_VAL_1;
	writel(val, crt_base + SCPU_DIV_OFFSET);
	val = readl(crt_base + SCPU_DIV_OFFSET);
	pr_info("RTD1295: After setting divider, DIV = 0x%08x (bits 7-8 = %d, should be 1)\n", 
		val, (val >> SCPU_DIV_SHIFT) & 0x3);

	/* Step 7: Small delay for PLL to stabilize (vendor uses __delay(100)) */
	udelay(100);

	/* Step 8: Switch clock source from OSC back to PLL */
	val = readl(crt_base + SYS_GROUP1_CK_SEL);
	pr_info("RTD1295: Before final switch, clock selector = 0x%08x (bit 2 = %s)\n", 
		val, (val & 0x4) ? "OSC" : "PLL");
	
	/* Clear bit 2 to select PLL */
	val &= ~0x4;
	writel(val, crt_base + SYS_GROUP1_CK_SEL);
	val = readl(crt_base + SYS_GROUP1_CK_SEL);
	pr_info("RTD1295: Switched to PLL (1400 MHz), clock selector = 0x%08x (bit 2 should be 0)\n", val);

	iounmap(crt_base);

	pr_info("RTD1295: CPU frequency configuration complete\n");
	
	/* Start monitor thread */
	monitor_thread = kthread_run(monitor_thread_fn, NULL, "rtd1295-monitor");
	if (IS_ERR(monitor_thread)) {
		pr_err("RTD1295: Failed to create monitor thread\n");
		return PTR_ERR(monitor_thread);
	}
	
	return 0;
}

static void __exit rtd1295_cpufreq_exit(void)
{
	pr_info("RTD1295: Stopping monitor thread\n");
	monitor_running = false;
	if (monitor_thread)
		kthread_stop(monitor_thread);
}

late_initcall(rtd1295_cpufreq_init);
module_exit(rtd1295_cpufreq_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("RTD1295 Optimization");
MODULE_DESCRIPTION("RTD1295 CPU Frequency Initialization and Monitor");
MODULE_VERSION("1.1");
