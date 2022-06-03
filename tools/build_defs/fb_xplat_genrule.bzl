# Only used for PyTorch open source BUCK build

def fb_xplat_genrule(default_outs = ["."], **kwargs):
    if repository_name() != "@":
        fail("This file is only for open source PyTorch build. Use the one in fbsource/tools instead!")

    genrule(
        # default_outs=default_outs, # only needed for internal BUCK
        **kwargs
    )
