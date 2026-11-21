static bool group_physical(hwc_display_contents_1_t *d,hwc_composer_device_1_t *dev,
                           size_t count,hwc_display_contents_1_t **lists,int *result)
{
    char mode[PROP_VALUE_MAX]={0};
    __system_property_get("waydroid.active_apps",mode);
    if(strcmp(mode,"Waydroid") || !get_usage) return false;
    hwc_layer_1_t *first=NULL,*last=NULL;
    bool cursor=false;
    for(size_t i=0;i<d->numHwLayers;i++) {
        hwc_layer_1_t *l=&d->hwLayers[i];
        if(l->compositionType==HWC_FRAMEBUFFER_TARGET) continue;
        if(l->flags&HWC_IS_CURSOR_LAYER) {cursor=true; continue;}
        uint64_t usage=0;
        if(cursor || (l->compositionType!=HWC_FRAMEBUFFER && l->compositionType!=HWC_OVERLAY) ||
           (!l->handle && !(l->flags&HWC_SKIP_LAYER)) ||
           (l->handle && (get_usage(*mapper,l->handle,&usage) || (usage&0x4000)))) return false;
        if(!first) first=l;
        last=l;
    }
    if(!first) return false;
    uint32_t a=first->flags,b=last->flags;
    first->compositionType=last->compositionType=HWC_FRAMEBUFFER;
    first->flags|=HWC_SKIP_LAYER;
    last->flags|=HWC_SKIP_LAYER;
    *result=vendor_prepare(dev,count,lists);
    first->flags=a;
    last->flags=b;
    return true;
}
