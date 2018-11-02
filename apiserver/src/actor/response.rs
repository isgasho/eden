// Copyright (c) 2018-present, Facebook, Inc.
// All Rights Reserved.
//
// This software may be used and distributed according to the terms of the
// GNU General Public License version 2 or any later version.

use std::result::Result;

use actix_web;
use actix_web::{Body, HttpRequest, HttpResponse, Json, Responder};
use bytes::Bytes;

use super::lfs::BatchResponse;
use super::model::{Changeset, Entry, EntryWithSizeAndContentHash};

pub enum MononokeRepoResponse {
    GetRawFile {
        content: Bytes,
    },
    GetBlobContent {
        content: Bytes,
    },
    ListDirectory {
        files: Box<Iterator<Item = Entry> + Send>,
    },
    GetTree {
        files: Vec<EntryWithSizeAndContentHash>,
    },
    GetChangeset {
        changeset: Changeset,
    },
    IsAncestor {
        answer: bool,
    },
    DownloadLargeFile {
        content: Bytes,
    },
    LfsBatch {
        response: BatchResponse,
    },
    UploadLargeFile {},
}

fn binary_response(content: Bytes) -> HttpResponse {
    HttpResponse::Ok()
        .content_type("application/octet-stream")
        .body(Body::Binary(content.into()))
}

impl Responder for MononokeRepoResponse {
    type Item = HttpResponse;
    type Error = actix_web::Error;

    fn respond_to<S: 'static>(self, req: &HttpRequest<S>) -> Result<Self::Item, Self::Error> {
        use self::MononokeRepoResponse::*;

        match self {
            GetRawFile { content } | GetBlobContent { content } => Ok(binary_response(content)),
            ListDirectory { files } => {
                Json(files.collect::<Vec<_>>()).respond_to(req)
            }
            GetTree{files}=> {
                Json(files).respond_to(req)
            }
            GetChangeset { changeset } => Json(changeset).respond_to(req),
            IsAncestor { answer } => Ok(binary_response({
                if answer {
                    "true".into()
                } else {
                    "false".into()
                }
            })),
            DownloadLargeFile { content } => Ok(binary_response(content.into())),
            LfsBatch { response } => Json(response).respond_to(req),
            UploadLargeFile {} => Ok(HttpResponse::Ok().into()),
        }
    }
}
