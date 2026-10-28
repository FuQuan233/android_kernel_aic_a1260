/* SPDX-License-Identifier: GPL-2.0-only */
/* Type-safe Linux 6.18 adapters. The firmware is single-link and STA-only. */
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
