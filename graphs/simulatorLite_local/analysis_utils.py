from ctypes import Structure, c_long
from datetime import datetime

import numpy as np

TIMEVAL_LEN = 16  # bytes


class timeval(Structure):
    """
    timeval struct from sys/time.h
    """
    _fields_ = [("tv_sec", c_long), ("tv_usec", c_long)]


class timespec(Structure):
    """
    timespec struct from sys/time.h
    """
    _fields_ = [("tv_sec", c_long), ("tv_nsec", c_long)]


def timeval_to_datetime(val):
    """
    Convert a C timeval object to a Python datetime

    Parameters
    ----------
    val : bytes
        timeval object encoded as bytes
    Returns
    -------
    datetime
        Python datetime object
    """
    ts = timeval.from_buffer_copy(val)
    timestamp = datetime.fromtimestamp(ts.tv_sec + ts.tv_usec * 1e-6)
    return timestamp


def timeval_to_timestamp(val):
    """
    Convert a C timeval object to a timestamp

    Parameters
    ----------
    val : bytes
        timeval object encoded as bytes
    Returns
    -------
    float
        timestamp (in seconds)
    """
    ts = timeval.from_buffer_copy(val)
    timestamp = ts.tv_sec + ts.tv_usec * 1e-6
    return timestamp


def timevals_to_timestamps(timevals_encoded):
    """
    Convert a list of C timeval objects to a list of timestamps (in seconds)

    Parameters
    ----------
    timevals_encoded : bytes
        timeval objects encoded as bytes
    Returns
    -------
    list
        List of timestamps in units of seconds
    """
    tlen = TIMEVAL_LEN
    n_timevals = int(len(timevals_encoded) / tlen)
    ts = [
        timeval_to_timestamp(timevals_encoded[i * tlen:(i + 1) * tlen])
        for i in range(n_timevals)
    ]
    return ts


def timespec_to_timestamp(val):
    """
    Convert a C timespec object to a timestamp (in seconds)

    Parameters
    ----------
    val : bytes
        timespec object encoded as bytes
    Returns
    -------
    float
        Time in seconds
    """
    ts = timespec.from_buffer_copy(val)
    timestamp = ts.tv_sec + ts.tv_nsec * 1e-9
    return timestamp


def get_lagged_features(data, n_history: int = 4):
    """
    Lag the data along the time axis. Stack the lagged versions of the data
    along the feature axis.

    Parameters
    ----------
    data : array of shape (n_samples, n_features)
        Data to be lagged
    n_history : int, optional
        Number of bins of history to include in the lagged data, by default 4

    Returns
    -------
    lagged_features : array of shape (n_samples, n_history * n_features)
        Lagged version of the original data
    """
    assert n_history >= 1, 'n_history must be greater than or equal to 1'
    lags = [None] * n_history
    for i in range(n_history):
        lags[i] = np.zeros_like(data)
        lags[i][i:, :] = data[:-i, :] if i > 0 else data
    lagged_features = np.hstack(lags)
    return lagged_features
