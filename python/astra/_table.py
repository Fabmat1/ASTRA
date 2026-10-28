"""Tables as pandas DataFrames when pandas is there, plain Python otherwise."""

try:
    import pandas as _pd
except ImportError:          # pandas is optional
    _pd = None


def have_pandas():
    return _pd is not None


def rows(records, columns=None, use_pandas=True):
    """A list of dicts as a DataFrame (pandas) or the list itself."""
    records = list(records)
    if use_pandas and _pd is not None:
        return _pd.DataFrame.from_records(records, columns=columns)
    return records


def cols(arrays, use_pandas=True):
    """A dict of equal-length numpy arrays as a DataFrame or the dict itself."""
    if use_pandas and _pd is not None:
        return _pd.DataFrame(arrays)
    return arrays
