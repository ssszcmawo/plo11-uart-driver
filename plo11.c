#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/serial_core.h>
#include <linux/tty.h>
#include <linux/tty_flip.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/slab.h>
#include <linux/interrupt.h>
#include <linux/kfifo.h>
#include <linux/init.h>

#define DEVICE_NAME     "plo11"
#define NR_UARTS        2

#define PLO11_CLK       24000000
#define PLO11_BR_MIN    (PLO11_CLK / (16 * 0x3ff))
#define PLO11_BR_MAX    (PLO11_CLK / (16 * 0x001))

#define PLO11_DR        0x000
#define PLO11_FR        0x018
#define PLO11_IBRD      0x024
#define PLO11_FBRD      0x028
#define PLO11_CR        0x030
#define PLO11_IMSC      0x038
#define PLO11_MIS       0x040
#define PLO11_ICR       0x044

#define TXFE            (1 << 7)
#define RXFF            (1 << 6)
#define TXFF            (1 << 5)
#define RXFE            (1 << 4)

#define RX_INT          (1 << 4)
#define TX_INT          (1 << 5)

#define ENUART          (1 << 0)

struct plo11_data {
    struct uart_port *port;
    int irq;
};

static struct uart_port plo11_ports[NR_UARTS];

static struct uart_driver plo11_uart_driver = {
    .owner          = THIS_MODULE,
    .driver_name    = "plo11",
    .dev_name       = DEVICE_NAME,
    .major          = 0,
    .minor          = 0,
    .nr             = NR_UARTS,
};

static void calc_divisor(unsigned int clk, unsigned int baud,
                         unsigned int *ibrd, unsigned int *fbrd)
{
    unsigned int div = clk / (16 * baud);
    unsigned int rem = clk % (16 * baud);
    unsigned int frac = ((rem * 64) + (baud / 2)) / baud;

    if (frac >= 64) {
        div++;
        frac = 0;
    }

    *ibrd = div;
    *fbrd = frac;
}

static void plo11_rx(struct uart_port *port)
{
    struct tty_port *tport = &port->state->port;
    u32 stat;

    while (!((stat = ioread32(port->membase + PLO11_FR)) & RXFE)) {
        unsigned char ch = ioread8(port->membase + PLO11_DR);
        port->icount.rx++;
        tty_insert_flip_char(tport, ch, TTY_NORMAL);
    }

    tty_flip_buffer_push(tport);
}

static void plo11_tx(struct uart_port *port)
{
    struct tty_port *tport = &port->state->port;
    unsigned char ch;

    while (!(ioread32(port->membase + PLO11_FR) & TXFF)) {

        if (port->x_char) {
            iowrite8(port->x_char, port->membase + PLO11_DR);
            port->x_char = 0;
            port->icount.tx++;
            continue;
        }

        if (kfifo_is_empty(&tport->xmit_fifo) || uart_tx_stopped(port))
            break;

        if (kfifo_out(&tport->xmit_fifo, &ch, 1) != 1)
            break;

        iowrite8(ch, port->membase + PLO11_DR);
        port->icount.tx++;

        if (kfifo_len(&tport->xmit_fifo) < WAKEUP_CHARS)
            uart_write_wakeup(port);
    }
}

static irqreturn_t plo11_irq(int irq, void *dev_id)
{
    struct uart_port *port = dev_id;
    u32 mis;

    mis = ioread32(port->membase + PLO11_MIS);

    if (!mis)
        return IRQ_NONE;

    if (mis & RX_INT)
        plo11_rx(port);

    if (mis & TX_INT)
        plo11_tx(port);

    iowrite32(mis, port->membase + PLO11_ICR);

    return IRQ_HANDLED;
}

static unsigned int plo11_tx_empty(struct uart_port *port)
{
    return (ioread32(port->membase + PLO11_FR) & TXFE) ?
        TIOCSER_TEMT : 0;
}

static void plo11_stop_tx(struct uart_port *port)
{
    u32 val = ioread32(port->membase + PLO11_IMSC);
    val &= ~TX_INT;
    iowrite32(val, port->membase + PLO11_IMSC);
}

static void plo11_start_tx(struct uart_port *port)
{
    u32 val = ioread32(port->membase + PLO11_IMSC);
    val |= TX_INT;
    iowrite32(val, port->membase + PLO11_IMSC);

    plo11_tx(port);
}

static void plo11_stop_rx(struct uart_port *port)
{
    u32 val = ioread32(port->membase + PLO11_IMSC);
    val &= ~RX_INT;
    iowrite32(val, port->membase + PLO11_IMSC);
}

static void plo11_break_ctl(struct uart_port *port, int ctl) {}

static unsigned int plo11_get_mctrl(struct uart_port *port)
{
    return TIOCM_CTS | TIOCM_DSR | TIOCM_CAR;
}

static int plo11_startup(struct uart_port *port)
{
    struct plo11_data *pdata = port->private_data;
    int ret;

    ret = request_irq(pdata->irq, plo11_irq, 0, DEVICE_NAME, port);
    if (ret) 
    {
	pr_err("Failed to request IRQ\n");
	return ret;
    }

    iowrite32(ENUART, port->membase + PLO11_CR);
    iowrite32(RX_INT, port->membase + PLO11_IMSC);

    return 0;
}

static void plo11_shutdown(struct uart_port *port)
{
    struct plo11_data *pdata = port->private_data;

    iowrite32(0, port->membase + PLO11_IMSC);
    free_irq(pdata->irq, port);
}

static void plo11_set_termios(struct uart_port *port,
                             struct ktermios *termios,
                             const struct ktermios *old)
{
    unsigned int baud, ibrd, fbrd;

    baud = uart_get_baud_rate(port, termios, old,
                              PLO11_BR_MIN, PLO11_BR_MAX);

    calc_divisor(PLO11_CLK, baud, &ibrd, &fbrd);

    iowrite32(ibrd, port->membase + PLO11_IBRD);
    iowrite32(fbrd, port->membase + PLO11_FBRD);

    uart_update_timeout(port, termios->c_cflag, baud);
}

static const char *plo11_type(struct uart_port *port)
{
    return "plo11";
}

static const struct uart_ops plo11_ops = {
    .tx_empty       = plo11_tx_empty,
    .get_mctrl      = plo11_get_mctrl,
    .stop_tx        = plo11_stop_tx,
    .start_tx       = plo11_start_tx,
    .stop_rx        = plo11_stop_rx,
    .break_ctl      = plo11_break_ctl,
    .startup        = plo11_startup,
    .shutdown       = plo11_shutdown,
    .set_termios    = plo11_set_termios,
    .type           = plo11_type,
};

static int plo11_probe(struct platform_device *pdev)
{
    struct resource *res;
    struct plo11_data *pdata;
    struct uart_port *port;
    int irq, id;

    id = of_alias_get_id(pdev->dev.of_node, "serial");

    if (id < 0)
	id = 0;

    pdata = devm_kzalloc(&pdev->dev, sizeof(*pdata), GFP_KERNEL);
    if (!pdata)
        return -ENOMEM;

    res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
    irq = platform_get_irq(pdev, 0);

    if (!res || irq < 0)
        return -ENODEV;

    if (!plo11_uart_driver.state)
        uart_register_driver(&plo11_uart_driver);

    port = &plo11_ports[id];

    port->mapbase = res->start;
    port->membase = devm_ioremap_resource(&pdev->dev, res);
    port->irq = irq;
    port->iotype = UPIO_MEM;
    port->fifosize = 16;
    port->ops = &plo11_ops;
    port->flags = UPF_BOOT_AUTOCONF;
    port->line = id;
    port->dev = &pdev->dev;
    port->private_data = pdata;

    pdata->port = port;
    pdata->irq = irq;

    platform_set_drvdata(pdev, port);

    pr_info("PLO11: probe called\n");

    return uart_add_one_port(&plo11_uart_driver, port);
}

static const struct of_device_id plo11_of_match[] = {
    { .compatible = "plo11" },
    {}
};

MODULE_DEVICE_TABLE(of, plo11_of_match);

static void plo11_remove(struct platform_device *pdev)
{
    struct uart_port *port = platform_get_drvdata(pdev);

    if (port)
        uart_remove_one_port(&plo11_uart_driver, port);
}

static struct platform_driver plo11_platform_driver = {
    .probe  = plo11_probe,
    .remove = plo11_remove,
    .driver = {
        .name = "plo11",
	.of_match_table = plo11_of_match,
    },
};

module_platform_driver(plo11_platform_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("PLO11 UART Custom Driver");
