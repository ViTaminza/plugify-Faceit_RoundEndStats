{
    global:
        Plugify_*;

        /* s2sdk binding slots (defined by s2sdk_bindings, filled by the plugify module) */
        __s2sdk_*;

        /* hudmenu binding slots (defined by hudmenu_bindings, filled by the plugify module);
           pisexcompat references them. */
        __hudmenu_*;

        /* No exported methods: Faceit_RoundEndStats exposes nothing to other plugins. */

    local: *;
};
