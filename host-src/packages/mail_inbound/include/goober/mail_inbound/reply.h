#pragma once

// A reply's own words (design/machine/783.ThesisCommunity.md § 2): the text above what it
// quotes, without the quote's header ("On ... wrote:"), the quoted lines, an Outlook header
// block, a signature after "-- ", or a "Sent from my phone" line.

#include <string>
#include <string_view>

namespace goober::mail::inbound {

// Lines from the first sign of quoted mail on are dropped; so is a line holding "reply above
// this line", which a box may put in what it sends. The result is trimmed.
std::string reply_text(std::string_view text);

}  // namespace goober::mail::inbound
