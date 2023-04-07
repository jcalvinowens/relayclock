/*
 * Latching Relay Clock Firmware
 *
 * Written by Calvin Owens <jcalvinowens@gmail.com>
 *
 * To the extent possible under law, I waive all copyright and related or
 * neighboring rights. You should have received a copy of the CC0 license along
 * with this work. If not, see http://creativecommons.org/publicdomain/zero/1.0
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <stm32f0xx.h>

extern void *memcpy(void *d, const void *s, unsigned int n);
#define barrier() do { asm volatile ("" ::: "memory"); } while (0)

struct gpio {
	GPIO_TypeDef *reg;
	uint8_t nr;
};

/*
 * Input state (8.4.5 GPIOx_IDR).
 */
static inline int get_gpio(const struct gpio *gpio)
{
	return gpio->reg->IDR & (1UL << gpio->nr);
}

/*
 * Output data (8.4.6 GPIOx_ODR).
 */
static inline void set_gpio(const struct gpio *gpio, int new)
{
	if (new)
		gpio->reg->ODR |= 1UL << gpio->nr;
	else
		gpio->reg->ODR &= ~(1UL << gpio->nr);
}

/*
 * Input mode (8.4.1 GPIOx_MODER == 0)
 * Pull down (8.4.4 GPIOx_PUPDR == 2)
 */
static void configure_gpio_input_pull_down(const struct gpio *gpio)
{
	gpio->reg->MODER &= ~(3UL << (gpio->nr * 2));
	gpio->reg->PUPDR |= 2UL << (gpio->nr * 2);
}

/*
 * Input mode (8.4.1 GPIOx_MODER == 0)
 * Pull up (8.4.4 GPIOx_PUPDR == 1)
 */
static void configure_gpio_input_pull_up(const struct gpio *gpio)
{
	gpio->reg->MODER &= ~(3UL << (gpio->nr * 2));
	gpio->reg->PUPDR |= 1UL << (gpio->nr * 2);
}

/*
 * Ouptut mode (8.4.1 GPIOx_MODER == 1)
 */
static void configure_gpio_pp(const struct gpio *gpio, int init)
{
	set_gpio(gpio, init);
	gpio->reg->MODER |= 1UL << (gpio->nr * 2);
}

/*
 * Empirical.
 */
static void busywait_ms(uint32_t wait)
{
	while (wait--) {
		uint32_t l = 1300;
		while (l--)
			barrier();
	}
}

/*
 * RTC write protection keys (see 21.4.7)
 */

static inline void rtc_unlock(void)
{
	PWR->CR |= PWR_CR_DBP;
	RTC->WPR = 0xCA;
	RTC->WPR = 0x53;
}

static inline void rtc_lock(void)
{
	RTC->WPR = 0xFE;
	RTC->WPR = 0x64;
	PWR->CR &= ~PWR_CR_DBP;
}

/*
 * The CALP bit adds 512 ticks per 32 second cycle, the CALM bits subract 0-511.
 * C = ((S_PER_DAY / 86400) * 32) / (1 / 32768)
 */
static void __configure_rtc_calibration(uint32_t c, int dir)
{
	if (dir && c > 0) {
		RTC->CALR = RTC_CALR_CALP | ((512UL - c) & RTC_CALR_CALM);
		return;
	}

	RTC->CALR = c & RTC_CALR_CALM;
}

/*
 * The flash.sh script sets these variables via GDB to program the current time.
 */

volatile uint8_t ready_set;
volatile int32_t CALB;
volatile uint8_t YR_T;
volatile uint8_t YR_O;
volatile uint8_t MO_T;
volatile uint8_t MO_O;
volatile uint8_t DY_T;
volatile uint8_t DY_O;
volatile uint8_t HR_T;
volatile uint8_t HR_O;
volatile uint8_t MN_T;
volatile uint8_t MN_O;
volatile uint8_t SC_T;
volatile uint8_t SC_O;

static void __rtc_time_init(void)
{
	while (ready_set == 0)
		barrier();

	RTC->ISR |= RTC_ISR_INIT;
	while (!(RTC->ISR & RTC_ISR_INITF))
		barrier();

	#ifdef USE_LSI
	RTC->PRER = 124UL << 16 | 328UL;
	#else
	RTC->PRER = 0x007F00FFUL;
	#endif

	RTC->DR = (YR_T) << 20 |
		  (YR_O) << 16 |
		  (MO_T) << 12 |
		  (MO_O) << 8 |
		  (DY_T) << 4 |
		  (DY_O);

	RTC->TR = (HR_T) << 20 |
		  (HR_O) << 16 |
		  (MN_T) << 12 |
		  (MN_O) << 8 |
		  (SC_T) << 4 |
		  (SC_O);

	if (CALB <= 0)
		__configure_rtc_calibration((uint32_t)-CALB, 0);
	else
		__configure_rtc_calibration((uint32_t)CALB, 1);

	RTC->ISR &= ~RTC_ISR_INIT;
	RTC->CR |= RTC_CR_BYPSHAD;
}

static void rtc_init(void)
{
	rtc_unlock();

	#ifdef USE_LSI
	RCC->CSR |= RCC_CSR_LSION;
	while (!(RCC->CSR & RCC_CSR_LSIRDY))
		barrier();

	RCC->BDCR |= RCC_BDCR_RTCSEL_LSI | RCC_BDCR_RTCEN | RTC_CR_OSEL_0;
	#else
	/*
	 * Enable the LSE crystal with the lowest drive strength setting, wait
	 * for it to stabilize.
	 */

	RCC->BDCR &= ~(RCC_BDCR_LSEDRV_0 | RCC_BDCR_LSEDRV_1);
	RCC->BDCR |= RCC_BDCR_LSEON;
	while (!(RCC->BDCR & RCC_BDCR_LSERDY))
		barrier();

	/*
	 * Configure LSE as the source, ALARM_A as the output.
	 */

	RCC->BDCR |= RCC_BDCR_RTCSEL_LSE | RCC_BDCR_RTCEN | RTC_CR_OSEL_0;
	#endif

	__rtc_time_init();

	rtc_lock();
}

static void configure_rtc_alarm_a(void)
{
	rtc_unlock();

	RTC->CR &= ~RTC_CR_ALRAE;
	while (RTC->CR & RTC_ISR_ALRAWF)
		barrier();

	/*
	 * Mask all matches except seconds, set match value to zero, enable.
	 */

	RTC->ALRMAR = RTC_ALRMAR_MSK4 | RTC_ALRMAR_MSK3 | RTC_ALRMAR_MSK2 | 0;
	RTC->CR |= RTC_CR_ALRAIE | RTC_CR_ALRAE;

	rtc_lock();
}

void RTC_IRQHandler(void)
{
	RTC->ISR &= ~RTC_ISR_ALRAF;
	EXTI->PR |= EXTI_PR_PR17;
}

/*
 * Since we don't use the tamper detection feature, we can use the RTC TAMPTS
 * bit as a flag preserved across STANDBY to indicate we need to latch all 28
 * relays next time we update the digits.
 */

static void force_full_relatch(void)
{
	if (RTC->TAFCR & RTC_TAFCR_TAMPTS)
		return;

	rtc_unlock();
	RTC->TAFCR |= RTC_TAFCR_TAMPTS;
	rtc_lock();
}

static int full_relatch_forced(void)
{
	if (!(RTC->TAFCR & RTC_TAFCR_TAMPTS))
		return 0;

	rtc_unlock();
	RTC->TAFCR &= ~RTC_TAFCR_TAMPTS;
	rtc_lock();
	return 1;
}

/*
 * Hardcoded California DST corrections.
 */

struct dst_corr {
	uint8_t y;
	uint8_t m;
	uint8_t d;
	uint8_t p; // 0 == SUB, 1 == ADD
};

static const struct dst_corr dst_corrs[] = {
	(struct dst_corr){.y = 23, .m =  3, .d = 12, .p = 1, },
	(struct dst_corr){.y = 23, .m = 11, .d =  5, .p = 0, },
	(struct dst_corr){.y = 24, .m =  3, .d = 10, .p = 1, },
	(struct dst_corr){.y = 24, .m = 11, .d =  3, .p = 0, },
	(struct dst_corr){.y = 25, .m =  3, .d =  9, .p = 1, },
	(struct dst_corr){.y = 25, .m = 11, .d =  2, .p = 0, },
};

static void handle_dst(void)
{
	int y, m, d;
	uint32_t dr;
	unsigned i;

	dr = RTC->DR;
	y = ((dr & RTC_DR_YT) >> 20) * 10 + ((dr & RTC_DR_YU) >> 16);
	m = ((dr & RTC_DR_MT) >> 12) * 10 + ((dr & RTC_DR_MU) >> 8);
	d = ((dr & RTC_DR_DT) >> 4) * 10 + ((dr & RTC_DR_DU));

	for (i = 0; i < sizeof(dst_corrs) / sizeof(struct dst_corr); i++) {
		const struct dst_corr *c = dst_corrs + i;

		if (y == c->y && m == c->m && d == c->d) {
			rtc_unlock();

			if (dst_corrs[i].p == 0) {
				/*
				 * In the SUB1H case, 1:59AM happens twice. The
				 * STM32 RTC has a state bit (CR_BKP) for this.
				 */
				if (RTC->CR & RTC_CR_BKP) {
					RTC->CR &= ~RTC_CR_BKP;
				} else {
					RTC->CR |= RTC_CR_BKP | RTC_CR_SUB1H;
				}
			} else {
				RTC->CR |= RTC_CR_ADD1H;
			}

			rtc_lock();
			return;
		}
	}
}

/*
 * LED Segment Layout (as viewed looking at the board):
 *
 *  (0)   (1)   (2)   (3)   <=== Index in led_map
 *
 * |-E-| |-E-| |-E-| |-E-|  <===\
 * F   D F   D F   D F   D  <===\\
 * |-G-| |-G-| |-G-| |-G-|  <=== Segment ID
 * A   C A   C A   C A   C  <===//
 * |-B-| |-B-| |-B-| |-B-|  <===/
 *
 * The COILP line controls 28 pairs of oppositely wired SPDT solid state
 * switches which function as H-bridges to set the direction of current pulse
 * through the relay coils.
 *
 * The 28 push-pull segment GPIOs are wired to bases of four Darlington array
 * packages, which sink current from the relay coils. The 5V LDO can probably
 * source enough current to sink them all at once, but to get the aesthetic
 * "clicky" effect an unnecessary delay is added between them.
 *
 * Latching all 28 relays every minute wastes power and makes the aesthetic
 * effect less nice. But, since we can't read the state of the relays, it's not
 * possible to know which ones actually need to be latched if the clock has been
 * unplugged.
 *
 * To handle this, the 5V LDO output is wired to a GPIO, and the code sets a
 * flag and skips latching if it detects it is unplugged when it wakes up to
 * update the clock digits. When the clock is plugged back in, the flag forces
 * all 28 relays to be latched, so the assumption of the relay state being the
 * prior minute's digits is always valid.
 */

struct led_segment {
	uint8_t a : 1;
	uint8_t b : 1;
	uint8_t c : 1;
	uint8_t d : 1;
	uint8_t e : 1;
	uint8_t f : 1;
	uint8_t g : 1;
};

static const struct led_segment led_font[20] = {
// Numbers: 0123456789
(struct led_segment){ .a = 1, .b = 1, .c = 1, .d = 1, .e = 1, .f = 1, },
(struct led_segment){ .c = 1, .d = 1, },
(struct led_segment){ .a = 1, .b = 1, .d = 1, .e = 1, .g = 1, },
(struct led_segment){ .b = 1, .c = 1, .d = 1, .e = 1, .g = 1, },
(struct led_segment){ .c = 1, .d = 1, .f = 1, .g = 1, },
(struct led_segment){ .b = 1, .c = 1, .e = 1, .f = 1, .g = 1, },
(struct led_segment){ .a = 1, .b = 1, .c = 1, .e = 1, .f = 1, .g = 1, },
(struct led_segment){ .c = 1, .d = 1, .e = 1, },
(struct led_segment){ .a = 1, .b = 1, .c = 1, .d = 1, .e = 1, .f = 1, .g = 1, },
(struct led_segment){ .c = 1, .d = 1, .e = 1, .f = 1, .g = 1, },
// Blank
(struct led_segment){ 0 },
// Letters: ACEFHLPU
(struct led_segment){ .a = 1, .c = 1, .d = 1, .e = 1, .f = 1, .g = 1, },
(struct led_segment){ .a = 1, .b = 1, .e = 1, .f = 1, },
(struct led_segment){ .a = 1, .b = 1, .e = 1, .f = 1, .g = 1, },
(struct led_segment){ .a = 1, .e = 1, .f = 1, .g = 1, },
(struct led_segment){ .a = 1, .c = 1, .d = 1, .f = 1, .g = 1, },
(struct led_segment){ .a = 1, .b = 1, .f = 1, },
(struct led_segment){ .a = 1, .d = 1, .e = 1, .f = 1, .g = 1, },
(struct led_segment){ .a = 1, .b = 1, .c = 1, .d = 1, .f = 1, },
// Hyphen
(struct led_segment){ .g = 1, },
};

struct led_segment_to_gpio_map {
	struct gpio seg_a;
	struct gpio seg_b;
	struct gpio seg_c;
	struct gpio seg_d;
	struct gpio seg_e;
	struct gpio seg_f;
	struct gpio seg_g;
};

static const struct led_segment_to_gpio_map led_map[4] = {
	(const struct led_segment_to_gpio_map){
		.seg_a = {.reg = GPIOB, .nr = 13, },
		.seg_b = {.reg = GPIOB, .nr = 14, },
		.seg_c = {.reg = GPIOB, .nr = 15, },
		.seg_d = {.reg = GPIOA, .nr =  8, },
		.seg_e = {.reg = GPIOA, .nr =  9, },
		.seg_f = {.reg = GPIOA, .nr = 10, },
		.seg_g = {.reg = GPIOA, .nr = 11, },
	},
	(const struct led_segment_to_gpio_map){
		.seg_a = {.reg = GPIOB, .nr = 12, },
		.seg_b = {.reg = GPIOB, .nr = 11, },
		.seg_c = {.reg = GPIOB, .nr = 10, },
		.seg_d = {.reg = GPIOB, .nr =  2, },
		.seg_e = {.reg = GPIOB, .nr =  1, },
		.seg_f = {.reg = GPIOB, .nr =  0, },
		.seg_g = {.reg = GPIOA, .nr =  7, },
	},
	(const struct led_segment_to_gpio_map){
		.seg_a = {.reg = GPIOA, .nr = 12, },
		.seg_b = {.reg = GPIOA, .nr = 15, },
		.seg_c = {.reg = GPIOB, .nr =  3, },
		.seg_d = {.reg = GPIOB, .nr =  4, },
		.seg_e = {.reg = GPIOB, .nr =  5, },
		.seg_f = {.reg = GPIOB, .nr =  6, },
		.seg_g = {.reg = GPIOB, .nr =  7, },
	},
	(const struct led_segment_to_gpio_map){
		.seg_a = {.reg = GPIOA, .nr =  6, },
		.seg_b = {.reg = GPIOA, .nr =  5, },
		.seg_c = {.reg = GPIOA, .nr =  4, },
		.seg_d = {.reg = GPIOA, .nr =  3, },
		.seg_e = {.reg = GPIOA, .nr =  2, },
		.seg_f = {.reg = GPIOA, .nr =  1, },
		.seg_g = {.reg = GPIOA, .nr =  0, },
	},
};

static const struct gpio gpio_plug_detect =	{.reg = GPIOB, .nr =  9, };
static const struct gpio gpio_coilp =		{.reg = GPIOB, .nr =  8, };
static const struct gpio gpio_sw1 =		{.reg = GPIOC, .nr = 13, };
static const struct gpio gpio_sw2 =		{.reg = GPIOF, .nr =  6, };
static const struct gpio gpio_sw3 =		{.reg = GPIOF, .nr =  7, };

static void set_segment_state(const struct gpio *gpio, uint8_t state)
{
	set_gpio(&gpio_coilp, state);
	busywait_ms(1);
	set_gpio(gpio, 1);
	busywait_ms(10);
	set_gpio(gpio, 0);
	busywait_ms(25);
}

static void draw_digit(int which, int new_digit, int old_digit)
{
	const struct led_segment_to_gpio_map *map = led_map + which;
	const struct led_segment *new_font = led_font + new_digit;
	const struct led_segment *old_font = led_font + old_digit;

	if (old_digit == -1 || (new_font->a ^ old_font->a))
		set_segment_state(&map->seg_a, new_font->a);

	if (old_digit == -1 || (new_font->b ^ old_font->b))
		set_segment_state(&map->seg_b, new_font->b);

	if (old_digit == -1 || (new_font->c ^ old_font->c))
		set_segment_state(&map->seg_c, new_font->c);

	if (old_digit == -1 || (new_font->d ^ old_font->d))
		set_segment_state(&map->seg_d, new_font->d);

	if (old_digit == -1 || (new_font->e ^ old_font->e))
		set_segment_state(&map->seg_e, new_font->e);

	if (old_digit == -1 || (new_font->f ^ old_font->f))
		set_segment_state(&map->seg_f, new_font->f);

	if (old_digit == -1 || (new_font->g ^ old_font->g))
		set_segment_state(&map->seg_g, new_font->g);
}

static void configure_initial_reset(void)
{
	draw_digit(0, 19, -1);
	draw_digit(1, 19, -1);
	draw_digit(2, 19, -1);
	draw_digit(3, 19, -1);

	rtc_init();
	force_full_relatch();
}

static void init_clocks(void)
{
	/*
	 * Reset everything.
	 */

	RCC->CR &= ~(RCC_CR_HSEON | RCC_CR_CSSON | RCC_CR_PLLON |
		     RCC_CR_HSEBYP);

	RCC->CR2 &= ~RCC_CR2_HSI14ON;

	RCC->CFGR &= ~(RCC_CFGR_SW | RCC_CFGR_HPRE | RCC_CFGR_PPRE |
		       RCC_CFGR_ADCPRE | RCC_CFGR_MCO | RCC_CFGR_PLLSRC |
		       RCC_CFGR_PLLXTPRE | RCC_CFGR_PLLMUL);

	RCC->CFGR2 &= ~RCC_CFGR2_PREDIV1;

	RCC->CFGR3 &= ~(RCC_CFGR3_USART1SW | RCC_CFGR3_I2C1SW |
			RCC_CFGR3_CECSW | RCC_CFGR3_ADCSW);

	/*
	 * No external flash is used.
	 */

	FLASH->ACR = FLASH_ACR_PRFTBE;
	RCC->CIR = 0;

	/*
	 * Enable the built-in "HSI" oscillator, and use it as the clock source.
	 */

	RCC->CR |= RCC_CR_HSION;
	while (!(RCC->CR & RCC_CR_HSIRDY))
		barrier();

	RCC->CFGR |= RCC_CFGR_HPRE_DIV1 | RCC_CFGR_PPRE_DIV1 | RCC_CFGR_SW_HSI;
	while ((RCC->CFGR & RCC_CFGR_SWS) != RCC_CFGR_SWS_HSI)
		barrier();
}

int main(void)
{
	int p_dig0 = -1, p_dig1 = -1, p_dig2 = -1, p_dig3 = -1;
	int dig0, dig1, dig2, dig3, i;
	uint32_t now;

	init_clocks();

	/*
	 * If we aren't using the LSI, make sure it stays off.
	 */

	#ifndef USE_LSI
	RCC->CSR &= ~RCC_CSR_LSION;
	#endif

	/*
	 * Configure GPIOs
	 */

	RCC->AHBENR |= RCC_AHBENR_GPIOAEN | RCC_AHBENR_GPIOBEN |
		       RCC_AHBENR_GPIOCEN | RCC_AHBENR_GPIOFEN;

	configure_gpio_input_pull_up(&gpio_sw1);
	configure_gpio_input_pull_up(&gpio_sw2);
	configure_gpio_input_pull_up(&gpio_sw3);

	configure_gpio_pp(&gpio_coilp, 0);
	for (i = 0; i < 4; i++) {
		configure_gpio_pp(&led_map[i].seg_a, 0);
		configure_gpio_pp(&led_map[i].seg_b, 0);
		configure_gpio_pp(&led_map[i].seg_c, 0);
		configure_gpio_pp(&led_map[i].seg_d, 0);
		configure_gpio_pp(&led_map[i].seg_e, 0);
		configure_gpio_pp(&led_map[i].seg_f, 0);
		configure_gpio_pp(&led_map[i].seg_g, 0);
	}

	/*
	 * The rev 1.1 board has a button on PB8, so invert the plug detect
	 * logic by pulling up instead of down.
	 */

	#ifdef OLD_PB9_BUTTON
	configure_gpio_input_pull_up(&gpio_plug_detect);
	#else
	configure_gpio_input_pull_down(&gpio_plug_detect);
	#endif

	/*
	 * If the user is holding down SW1, stop (for ease of flashing).
	 */

	for (i = 0; !get_gpio(&gpio_sw1); i++) {
		while (i == 10)
			barrier();
	}

	/*
	 * Exiting STANDBY looks exactly like a initial power-on reset, except
	 * for the SBF bit in PWR_CSR (see RM0360 6.3.5). We only want to reset
	 * the RTC if this is the initial power up.
	 */

	RCC->APB1ENR |= RCC_APB1ENR_PWREN;
	if (!(PWR->CSR & PWR_CSR_SBF))
		configure_initial_reset();

	PWR->CR |= PWR_CR_CSBF | PWR_CR_CWUF;

	/*
	 * Read the current time from the RTC.
	 */

	now = RTC->TR;
	dig0 = (now & RTC_TR_HT) >> 20;
	dig1 = (now & RTC_TR_HU) >> 16;
	dig2 = (now & RTC_TR_MNT) >> 12;
	dig3 = (now & RTC_TR_MNU) >> 8;

	/*
	 * If the clock is unplugged, skip latching the relays.
	 */

	for (i = 0; !get_gpio(&gpio_plug_detect); i++) {
		if (i == 10) {
			force_full_relatch();
			goto unplugged;
		}
	}

	/*
	 * Most of the time, we know what the clock currently says, so only
	 * trigger the relays that actually need to change.
	 */

	if (!full_relatch_forced()) {
		p_dig0 = dig0;
		p_dig1 = dig1;
		p_dig2 = dig2;

		if (dig3 == 0) {
			p_dig3 = 9;
			if (dig2 == 0) {
				p_dig2 = 5;
				if (dig1 == 0) {
					switch (dig0) {
						case 0: p_dig1 = 3; break;
						case 1: p_dig1 = 9; break;
						case 2: p_dig1 = 9; break;
					}

					if (dig0 == 0) {
						p_dig0 = 2;
					} else {
						p_dig0 = dig0 - 1;
					}
				} else {
					p_dig1 = dig1 - 1;
				}
			} else {
				p_dig2 = dig2 - 1;
			}
		} else {
			p_dig3 = dig3 - 1;
		}
	}

	draw_digit(0, dig0, p_dig0);
	draw_digit(1, dig1, p_dig1);
	draw_digit(2, dig2, p_dig2);
	draw_digit(3, dig3, p_dig3);

	/*
	 * If it is 1:59AM, we might need to apply a DST correction...
	 */
unplugged:
	if ((dig0 == 0) && (dig1 == 1) && (dig2 == 5) && (dig3 == 9))
		handle_dst();

	/*
	 * Configure RTC ALARM A to fire at :00 of the next minute.
	 */

	EXTI->IMR |= EXTI_IMR_MR17;
	EXTI->RTSR |= EXTI_RTSR_TR17;
	NVIC_EnableIRQ(RTC_IRQn);
	NVIC_SetPriority(RTC_IRQn, 0);
	configure_rtc_alarm_a();

	/*
	 * Enter the deepest sleep state (STANDBY).
	 */

	SCB->SCR |= SCB_SCR_SLEEPDEEP_Msk;
	PWR->CR |= PWR_CR_PDDS | PWR_CR_LPDS;
	asm volatile ("wfi" ::: "cc", "memory");
	__builtin_unreachable();
}
