% Generated manually
\name{init_help_pane}
\alias{init_help_pane}
\title{Initialise the silent help-pane channel}
\usage{
init_help_pane(port_file, queue_file, url_file, interval = 0.15)
}
\arguments{
\item{port_file}{Path where the httpd port should be written.}
\item{queue_file}{Path the IDE writes topic requests into.}
\item{url_file}{Path where the resolved URL is written for the IDE.}
\item{interval}{Polling interval in seconds (only used with \code{later}).}
}
\description{
Starts the dynamic help server, writes its port to \code{port_file}, and
begins polling \code{queue_file} for help-topic requests from the Q IDE
frontend. Each line in \code{queue_file} is treated as a topic; the
resulting URL is written to \code{url_file}. Polling is done via
\code{later::later()} so it runs in R's idle event loop and produces no
console output.
}
