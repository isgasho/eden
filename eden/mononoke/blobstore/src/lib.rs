/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This software may be used and distributed according to the terms of the
 * GNU General Public License version 2.
 */

#![deny(warnings)]

use auto_impl::auto_impl;
use bytes::Bytes;
use std::fmt;

use anyhow::Error;
use futures::future::{self, Future};
use futures_ext::{BoxFuture, FutureExt};
use thiserror::Error;

use context::CoreContext;

mod counted_blobstore;
pub use crate::counted_blobstore::CountedBlobstore;

mod errors;
pub use crate::errors::ErrorKind;

mod disabled;
pub use crate::disabled::DisabledBlob;

/// A type representing bytes written to or read from a blobstore. The goal here is to ensure
/// that only types that implement `From<BlobstoreBytes>` and `Into<BlobstoreBytes>` can be
/// stored in the blob store.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct BlobstoreBytes(Bytes);

impl BlobstoreBytes {
    #[inline]
    pub fn len(&self) -> usize {
        self.0.len()
    }

    /// This should only be used by blobstore and From/Into<BlobstoreBytes> implementations.
    #[inline]
    pub fn from_bytes<B: Into<Bytes>>(bytes: B) -> Self {
        BlobstoreBytes(bytes.into())
    }

    /// This should only be used by blobstore and From/Into<BlobstoreBytes> implementations.
    #[inline]
    pub fn into_bytes(self) -> Bytes {
        self.0
    }

    /// This should only be used by blobstore and From/Into<BlobstoreBytes> implementations.
    #[inline]
    pub fn as_bytes(&self) -> &Bytes {
        &self.0
    }
}

/// The blobstore interface, shared across all blobstores.
/// A blobstore must provide the following guarantees:
/// 1. `get` and `put` are atomic with respect to each other; a put will either put the entire
///    value, or not put anything, and a get will return either None, or the entire value that an
///    earlier put inserted.
/// 2. Once the future returned by `put` completes, the data is durably stored. This implies that
///    a permanent failure of the backend will not lose the data unless multiple replicas in the
///    backend are lost. For example, if you have replicas in multiple datacentres, you will
///    not lose data until you lose two or more datacentres. However, losing replicas can make the
///    data inaccessible for a time.
/// 3. Once the future returned by `put` completes, calling `get` from any process will get you a
///    future that will return the data that was saved in the blobstore; this is so that after the
///    `put` completes, Mononoke can update a database table and be confident that all Mononoke
///    instances can `get` the blobs that the database refers to.
///
/// Implementations of this trait can assume that the same value is supplied if two keys are
/// equal - in other words, each key is associated with at most one globally unique value.
/// In other words, `put(key, value).and_then(put(key, value2))` implies `value == value2` for the
/// `BlobstoreBytes` definition of equality. If `value != value2`, then the implementation's
/// behaviour is implementation defined (it can overwrite or not write at all, as long as it does
/// not break the atomicity guarantee, and does not have to be consistent in its behaviour).
///
/// Implementations of Blobstore must be `Clone` if they are to interoperate with other Mononoke
/// uses of Blobstores
#[auto_impl(Arc, Box)]
pub trait Blobstore: fmt::Debug + Send + Sync + 'static {
    /// Fetch the value associated with `key`, or None if no value is present
    fn get(&self, ctx: CoreContext, key: String) -> BoxFuture<Option<BlobstoreBytes>, Error>;
    /// Associate `value` with `key` for future gets; if `put` is called with different `value`s
    /// for the same key, the implementation may return any `value` it's been given in response
    /// to a `get` for that `key`.
    fn put(&self, ctx: CoreContext, key: String, value: BlobstoreBytes) -> BoxFuture<(), Error>;
    /// Check that `get` will return a value for a given `key`, and not None. The provided
    /// implentation just calls `get`, and discards the return value; this can be overridden to
    /// avoid transferring data. In the absence of concurrent `put` calls, this must return
    /// `false` if `get` would return `None`, and `true` if `get` would return `Some(_)`.
    fn is_present(&self, ctx: CoreContext, key: String) -> BoxFuture<bool, Error> {
        self.get(ctx, key).map(|opt| opt.is_some()).boxify()
    }
    /// Errors if a given `key` is not present in the blob store. Useful to abort a chained
    /// future computation early if it cannot succeed unless the `key` is present
    fn assert_present(&self, ctx: CoreContext, key: String) -> BoxFuture<(), Error> {
        self.is_present(ctx, key.clone())
            .and_then(|present| {
                if present {
                    future::ok(())
                } else {
                    future::err(ErrorKind::NotFound(key).into())
                }
            })
            .boxify()
    }
}

#[derive(Debug, Error)]
pub enum LoadableError {
    #[error("Blobstore error")]
    Error(#[from] Error),
    #[error("Blob is missing: {0}")]
    Missing(String),
}

pub trait Loadable: Sized + 'static {
    type Value;

    fn load<B: Blobstore + Clone>(
        &self,
        ctx: CoreContext,
        blobstore: &B,
    ) -> BoxFuture<Self::Value, LoadableError>;
}

pub trait Storable: Sized + 'static {
    type Key;

    fn store<B: Blobstore + Clone>(
        self,
        ctx: CoreContext,
        blobstore: &B,
    ) -> BoxFuture<Self::Key, Error>;
}

/// StoreLoadable represents an object that be loaded asynchronously through a given store of type
/// S. This offers a bit more flexibility over Blobstore's Loadable, which requires that the object
/// be asynchronously load loadable from a Blobstore. This level of indirection allows for using
/// Manifest's implementations with Manifests that are not backed by a Blobstore.
pub trait StoreLoadable<S> {
    type Value;

    fn load(&self, ctx: CoreContext, store: &S) -> BoxFuture<Self::Value, LoadableError>;
}

/// For convenience, all Blobstore Loadables are StoreLoadable through any Blobstore.
impl<L: Loadable, S: Blobstore + Clone> StoreLoadable<S> for L {
    type Value = <L as Loadable>::Value;

    fn load(&self, ctx: CoreContext, store: &S) -> BoxFuture<Self::Value, LoadableError> {
        self.load(ctx, store).boxify()
    }
}
