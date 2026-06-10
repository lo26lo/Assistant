# =============================================================================
#  Vendorise depuis Pokemon-Dataset-Creator (github.com/lo26lo/Pokemon-Dataset-Creator)
#  core/utils.py — seul safe_print est repris (le reste etait specifique cartes).
# =============================================================================


def safe_print(*args, **kwargs):
    """
    Print avec gestion d'encodage pour Windows.
    Évite les erreurs UnicodeEncodeError avec les emojis sur console Windows.
    Accepte les mêmes arguments que print().
    """
    try:
        print(*args, **kwargs)
    except UnicodeEncodeError:
        safe_args = []
        for arg in args:
            if isinstance(arg, str):
                safe_args.append(arg.encode('ascii', 'ignore').decode('ascii'))
            else:
                safe_args.append(arg)
        print(*safe_args, **kwargs)
