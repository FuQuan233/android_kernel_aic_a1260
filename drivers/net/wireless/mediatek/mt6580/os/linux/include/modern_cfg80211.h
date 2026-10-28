/* SPDX-License-Identifier: GPL-2.0-only */
/* Type-safe Linux 6.18 adapters. The firmware is single-link and STA-only. */
#ifdef CONFIG_PM
/* NL80211's ANY mode retains normal firmware operation and wakes the AP
 * on its ordinary HIF interrupt. No packet pattern, GTK/ARP offload or
 * CONN/DDR power removal is implied. PSM still uses real firmware events.
 */
static void mt6580_set_wakeup(struct wiphy *w, bool enabled)
{
    P_GLUE_INFO_T g = wiphy_priv(w);

    device_set_wakeup_enable(g->rHifInfo.Dev, enabled);
}

static int mt6580_suspend(struct wiphy *w, struct cfg80211_wowlan *wow)
{
    P_GLUE_INFO_T g = wiphy_priv(w);
    GL_HIF_INFO_T *h = &g->rHifInfo;
    int error;

    if (!wow)
        return 0; /* cfg80211 has already left the network. */
    if (!wow->any || !device_may_wakeup(h->Dev))
        return -EOPNOTSUPP;
    if (h->irq <= 0 || !g->u4ReadyFlag || (g->ulFlag & GLUE_FLAG_HALT))
        return -ENODEV;
    if (g->prScanRequest)
        return -EBUSY;
    if (h->wake_armed)
        return -EBUSY;
    error = enable_irq_wake(h->irq);
    h->pm_error = error;
    if (error)
        return error;
    h->pm_fw_owned = READ_ONCE(g->prAdapter->fgIsFwOwn);
    h->pm_sleepy = READ_ONCE(g->prAdapter->fgWiFiInSleepyState);
    h->pm_flags = READ_ONCE(g->ulFlag);
    h->pm_whlpcr = HIF_REG_READL(h, MCR_WHLPCR);
    WRITE_ONCE(h->wake_armed, true);
    h->pm_suspends++;
    return 0;
}

static int mt6580_resume(struct wiphy *w)
{
    P_GLUE_INFO_T g = wiphy_priv(w);
    GL_HIF_INFO_T *h = &g->rHifInfo;
    int error;

    if (!h->wake_armed)
        return 0;
    error = disable_irq_wake(h->irq);
    h->pm_error = error;
    if (error)
        return error;
    WRITE_ONCE(h->wake_armed, false);
    h->pm_resumes++;
    return 0;
}
#endif

/* Cached observations only: a diagnostic read must not wake the radio or
 * read HIF registers while the normal firmware ownership state can change.
 */
static ssize_t pm_state_show(struct device *dev,
    struct device_attribute *attribute, char *buffer)
{
    struct net_device *n = to_net_dev(dev);
    P_GLUE_INFO_T g = wiphy_priv(n->ieee80211_ptr->wiphy);
    GL_HIF_INFO_T *h = &g->rHifInfo;

    return sysfs_emit(buffer,
        "wake_armed=%u irq=%d suspends=%u resumes=%u irqs_while_armed=%u error=%d suspend_fw_owned=%u suspend_sleepy=%u suspend_flags=%08x suspend_whlpcr=%08x fw_owned=%u sleepy=%u power_refs=%u\n",
        READ_ONCE(h->wake_armed), h->irq, READ_ONCE(h->pm_suspends),
        READ_ONCE(h->pm_resumes), READ_ONCE(h->pm_irqs), READ_ONCE(h->pm_error),
        READ_ONCE(h->pm_fw_owned), READ_ONCE(h->pm_sleepy), READ_ONCE(h->pm_flags),
        READ_ONCE(h->pm_whlpcr), READ_ONCE(g->prAdapter->fgIsFwOwn),
        READ_ONCE(g->prAdapter->fgWiFiInSleepyState),
        READ_ONCE(g->prAdapter->u4PwrCtrlBlockCnt));
}
static DEVICE_ATTR_RO(pm_state);

static int mt6580_change_iface(struct wiphy *w, struct net_device *n,
    enum nl80211_iftype type, struct vif_params *p)
{
    if (type != NL80211_IFTYPE_STATION) return -EOPNOTSUPP;
    return mtk_cfg80211_change_iface(w, n, type, NULL, p);
}
static int mt6580_add_key(struct wiphy *w, struct wireless_dev *d, int link,
    u8 index, bool pairwise, const u8 *mac, struct key_params *p)
{
    if (link > 0 || !d->netdev) return -EOPNOTSUPP;
    return mtk_cfg80211_add_key(w, d->netdev, index, pairwise, mac, p);
}
static int mt6580_get_key(struct wiphy *w, struct wireless_dev *d, int link,
    u8 index, bool pairwise, const u8 *mac, void *cookie,
    void (*callback)(void *, struct key_params *))
{
    if (link > 0 || !d->netdev) return -EOPNOTSUPP;
    return mtk_cfg80211_get_key(w, d->netdev, index, pairwise, mac, cookie, callback);
}
static int mt6580_del_key(struct wiphy *w, struct wireless_dev *d, int link,
    u8 index, bool pairwise, const u8 *mac)
{
    if (link > 0 || !d->netdev) return -EOPNOTSUPP;
    return mtk_cfg80211_del_key(w, d->netdev, index, pairwise, mac);
}
static int mt6580_default_key(struct wiphy *w, struct net_device *n, int link,
    u8 index, bool unicast, bool multicast)
{
    if (link > 0) return -EOPNOTSUPP;
    return mtk_cfg80211_set_default_key(w, n, index, unicast, multicast);
}
static int mt6580_default_mgmt_key(struct wiphy *w, struct wireless_dev *d,
    int link, u8 index)
{
    return -EOPNOTSUPP; /* This BSP has no protected-management-frame support. */
}
static int mt6580_get_station(struct wiphy *w, struct wireless_dev *d,
    const u8 *mac, struct station_info *s)
{
    if (!d->netdev) return -EOPNOTSUPP;
    return mtk_cfg80211_get_station(w, d->netdev, (u8 *)mac, s);
}
static int mt6580_remain_on_channel(struct wiphy *w, struct wireless_dev *d,
    struct ieee80211_channel *c, unsigned int duration, u64 *cookie,
    const u8 *rx_addr)
{
    if (rx_addr) return -EOPNOTSUPP;
    return mtk_cfg80211_remain_on_channel(w, d, c, duration, cookie);
}
static int mt6580_mgmt_tx(struct wiphy *w, struct wireless_dev *d,
    struct cfg80211_mgmt_tx_params *p, u64 *cookie)
{
    return mtk_cfg80211_mgmt_tx(w, d, p->chan, p->offchan, p->wait,
        p->buf, p->len, p->no_cck, p->dont_wait_for_ack, cookie);
}
static void mt6580_mgmt_registrations(struct wiphy *w, struct wireless_dev *d,
    struct mgmt_frame_regs *regs)
{
    mtk_cfg80211_mgmt_frame_register(w, d, MAC_FRAME_PROBE_REQ,
        regs->interface_stypes & BIT(IEEE80211_STYPE_PROBE_REQ >> 4));
    mtk_cfg80211_mgmt_frame_register(w, d, MAC_FRAME_ACTION,
        regs->interface_stypes & BIT(IEEE80211_STYPE_ACTION >> 4));
}
