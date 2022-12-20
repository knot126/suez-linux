// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2022 Google Corporation
 */

#ifndef __COREDUMP_H
#define __COREDUMP_H

#define DEVCOREDUMP_TIMEOUT	msecs_to_jiffies(10000)	/* 10 sec */

typedef bool (*coredump_enabled_t)(struct hci_dev *hdev);
typedef void (*coredump_t)(struct hci_dev *hdev);
typedef int  (*dmp_hdr_t)(struct hci_dev *hdev, char *buf, size_t size);
typedef void (*notify_change_t)(struct hci_dev *hdev, int state);

/* struct hci_devcoredump - Devcoredump state
 *
 * @supported: Indicates if FW dump collection is supported by driver
 * @state: Current state of dump collection
 * @alloc_size: Total size of the dump
 * @head: Start of the dump
 * @tail: Pointer to current end of dump
 * @end: head + alloc_size for easy comparisons
 *
 * @dump_q: Dump queue for state machine to process
 * @dump_rx: Devcoredump state machine work
 * @dump_timeout: Devcoredump timeout work
 *
 * @enabled: Checks if the devcoredump is enabled for the device
 *
 * @coredump: Called from the driver's .coredump() function.
 * @dmp_hdr: Create a dump header to identify controller/fw/driver info
 * @notify_change: Notify driver when devcoredump state has changed
 */
struct hci_devcoredump {
	bool		supported;

	enum devcoredump_state {
		HCI_DEVCOREDUMP_IDLE,
		HCI_DEVCOREDUMP_ACTIVE,
		HCI_DEVCOREDUMP_DONE,
		HCI_DEVCOREDUMP_ABORT,
		HCI_DEVCOREDUMP_TIMEOUT
	} state;

	size_t		alloc_size;
	char		*head;
	char		*tail;
	char		*end;

	struct sk_buff_head	dump_q;
	struct work_struct	dump_rx;
	struct delayed_work	dump_timeout;

	coredump_enabled_t	enabled;

	coredump_t		coredump;
	dmp_hdr_t		dmp_hdr;
	notify_change_t		notify_change;
};

#ifdef CONFIG_DEV_COREDUMP

void hci_devcoredump_reset(struct hci_dev *hdev);
void hci_devcoredump_rx(struct work_struct *work);
void hci_devcoredump_timeout(struct work_struct *work);

int hci_devcoredump_register(struct hci_dev *hdev, coredump_t coredump,
			     dmp_hdr_t dmp_hdr, notify_change_t notify_change);
int hci_devcoredump_init(struct hci_dev *hdev, u32 dmp_size);
int hci_devcoredump_append(struct hci_dev *hdev, struct sk_buff *skb);
int hci_devcoredump_append_pattern(struct hci_dev *hdev, u8 pattern, u32 len);
int hci_devcoredump_complete(struct hci_dev *hdev);
int hci_devcoredump_abort(struct hci_dev *hdev);

#else

static inline void hci_devcoredump_reset(struct hci_dev *hdev) {}
static inline void hci_devcoredump_rx(struct work_struct *work) {}
static inline void hci_devcoredump_timeout(struct work_struct *work) {}

static inline int hci_devcoredump_register(struct hci_dev *hdev,
					   coredump_t coredump,
					   dmp_hdr_t dmp_hdr,
					   notify_change_t notify_change)
{
	return -EOPNOTSUPP;
}

static inline int hci_devcoredump_init(struct hci_dev *hdev, u32 dmp_size)
{
	return -EOPNOTSUPP;
}

static inline int hci_devcoredump_append(struct hci_dev *hdev,
					 struct sk_buff *skb)
{
	return -EOPNOTSUPP;
}

static inline int hci_devcoredump_append_pattern(struct hci_dev *hdev,
						 u8 pattern, u32 len)
{
	return -EOPNOTSUPP;
}

static inline int hci_devcoredump_complete(struct hci_dev *hdev)
{
	return -EOPNOTSUPP;
}

static inline int hci_devcoredump_abort(struct hci_dev *hdev)
{
	return -EOPNOTSUPP;
}

#endif /* CONFIG_DEV_COREDUMP */

#endif /* __COREDUMP_H */
