#include <asm/div64.h>
#include <linux/reciprocal_div.h>

/**
 * reciprocal_value:计算k的倒数
 */
u32 reciprocal_value(u32 k)
{
	u64 val = (1LL << 32) + (k - 1);
	do_div(val, k);
	return (u32)val;
}
