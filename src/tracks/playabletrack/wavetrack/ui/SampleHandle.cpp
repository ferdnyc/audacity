/**********************************************************************

Audacity: A Digital Audio Editor

SampleHandle.cpp

Paul Licameli split from TrackPanel.cpp

**********************************************************************/


#include "SampleHandle.h"

#include <algorithm>
#include <wx/gdicmn.h>

#include "Envelope.h"
#include "../../../../HitTestResult.h"
#include "../../../../prefs/WaveformSettings.h"
#include "ProjectAudioIO.h"
#include "ProjectHistory.h"
#include "../../../../RefreshCode.h"
#include "../../../../TrackArt.h"
#include "../../../../TrackArtist.h"
#include "../../../../TrackPanelMouseEvent.h"
#include "UndoManager.h"
#include "ViewInfo.h"
#include "WaveClip.h"
#include "WaveTrack.h"
#include "../../../../../images/Cursors.h"
#include "AudacityMessageBox.h"


static const int SMOOTHING_KERNEL_RADIUS = 3;
static const int SMOOTHING_BRUSH_RADIUS = 5;
static const double SMOOTHING_PROPORTION_MAX = 0.7;
static const double SMOOTHING_PROPORTION_MIN = 0.0;

SampleHandle::SampleHandle( const std::shared_ptr<WaveTrack> &pTrack )
   : mClickedTrack{ pTrack }
{
}

void SampleHandle::Enter(bool, AudacityProject *)
{
#ifdef EXPERIMENTAL_TRACK_PANEL_HIGHLIGHTING
   mChangeHighlight = RefreshCode::RefreshCell;
#endif
}

HitTestPreview SampleHandle::HitPreview
(const wxMouseState &state, const AudacityProject *WXUNUSED(pProject), bool unsafe)
{
   static auto disabledCursor =
      ::MakeCursor(wxCURSOR_NO_ENTRY, DisabledCursorXpm, 16, 16);
   static wxCursor smoothCursor{ wxCURSOR_SPRAYCAN };
   static auto pencilCursor =
      ::MakeCursor(wxCURSOR_PENCIL, DrawCursorXpm, 12, 22);

   // TODO:  message should also mention the brush.  Describing the modifier key
   // (alt, or other) varies with operating system.
   auto message = XO("Click and drag to edit the samples");

   return {
      message,
      (unsafe
       ? &*disabledCursor
       : (state.AltDown()
          ? &smoothCursor
          : &*pencilCursor))
   };
}

UIHandlePtr SampleHandle::HitAnywhere
(std::weak_ptr<SampleHandle> &holder,
 const wxMouseState &WXUNUSED(state), const std::shared_ptr<WaveTrack> &pTrack)
{
   auto result = std::make_shared<SampleHandle>( pTrack );
   result = AssignUIHandlePtr(holder, result);
   return result;
}

namespace {
   inline double adjustTime(const WaveTrack *wt, double time)
   {
      // Round to an exact sample time
      const auto clip = wt->GetClipAtTime(time);
      if (!clip)
         return wt->SnapToSample(time);
      const auto sampleOffset =
         clip->TimeToSamples(time - clip->GetPlayStartTime());
      return clip->SamplesToTime(sampleOffset) + clip->GetPlayStartTime();
   }

   // Is the sample horizontally nearest to the cursor sufficiently separated
   // from its neighbors that the pencil tool should be allowed to drag it?
   bool SampleResolutionTest
      ( const ViewInfo &viewInfo, const WaveTrack *wt, double time, int width )
   {
      // Require more than 3 pixels per sample
      const auto xx = std::max<ZoomInfo::int64>(0, viewInfo.TimeToPosition(time));
      const auto clip = wt->GetClipAtTime(time);
      if (!clip)
         // Don't bother the user about that with a pop-up.
         return true;
      ZoomInfo::Intervals intervals;
      const double rate = clip->GetRate() / clip->GetStretchRatio();
      viewInfo.FindIntervals(intervals, width);
      ZoomInfo::Intervals::const_iterator it = intervals.begin(),
         end = intervals.end(), prev;
      wxASSERT(it != end && it->position == 0);
      do
         prev = it++;
      while (it != end && it->position <= xx);
      const double threshold = 3 * rate;
      // three times as many pixels per second, as samples
      return prev->averageZoom > threshold;
   }
}

UIHandlePtr SampleHandle::HitTest
(std::weak_ptr<SampleHandle> &holder,
 const wxMouseState &state, const wxRect &rect,
 const AudacityProject *pProject, const std::shared_ptr<WaveTrack> &pTrack)
{
   const auto &viewInfo = ViewInfo::Get( *pProject );

   /// method that tells us if the mouse event landed on an
   /// editable sample
   const auto wavetrack = pTrack.get();
   const auto time = viewInfo.PositionToTime(state.m_x, rect.x);

   const double tt = adjustTime(wavetrack, time);
   if (!SampleResolutionTest(viewInfo, wavetrack, tt, rect.width))
      return {};

   // Just get one sample.
   float oneSample;
   constexpr auto iChannel = 0u;
   constexpr auto mayThrow = false;
   if (!wavetrack->GetFloatAtTime(tt, iChannel, oneSample, mayThrow))
      return {};

   // Get y distance of envelope point from center line (in pixels).
   auto &cache = WaveformScale::Get(*wavetrack);
   float zoomMin, zoomMax;
   cache.GetDisplayBounds(zoomMin, zoomMax);

   double envValue = 1.0;
   Envelope* env = wavetrack->GetEnvelopeAtTime(time);
   if (env)
      // Calculate sample as it would be rendered
      envValue = env->GetValue(tt);

   auto &settings = WaveformSettings::Get(*wavetrack);
   const bool dB = !settings.isLinear();
   int yValue = GetWaveYPos(oneSample * envValue,
      zoomMin, zoomMax,
      rect.height, dB, true,
      settings.dBRange, false) + rect.y;

   // Get y position of mouse (in pixels)
   int yMouse = state.m_y;

   // Perhaps yTolerance should be put into preferences?
   const int yTolerance = 10; // More tolerance on samples than on envelope.
   if (abs(yValue - yMouse) >= yTolerance)
      return {};

   return HitAnywhere(holder, state, pTrack);
}

SampleHandle::~SampleHandle()
{
}

namespace {
   /// Determines if we can edit samples in a wave track.
   /// Also pops up warning messages in certain cases where we can't.
   ///  @return true if we can edit the samples, false otherwise.
   bool IsSampleEditingPossible
      (const wxMouseEvent &event,
       const wxRect &rect, const ViewInfo &viewInfo, WaveTrack *wt, int width)
   {
      //If we aren't zoomed in far enough, show a message dialog.
      const double time = adjustTime(wt, viewInfo.PositionToTime(event.m_x, rect.x));
      if (!SampleResolutionTest(viewInfo, wt, time, width))
      {
         AudacityMessageBox(
            XO(
"To use Draw, zoom in further until you can see the individual samples."),
            XO("Draw Tool"));
         return false;
      }
      return true;
   }
}

UIHandle::Result SampleHandle::Click
(const TrackPanelMouseEvent &evt, AudacityProject *pProject)
{
   using namespace RefreshCode;
   const bool unsafe = ProjectAudioIO::Get( *pProject ).IsAudioActive();
   if ( unsafe )
      return Cancelled;

   const wxMouseEvent &event = evt.event;
   const wxRect &rect = evt.rect;
   const auto &viewInfo = ViewInfo::Get( *pProject );

   const double t0 = viewInfo.PositionToTime(event.m_x, rect.x);
   const auto pTrack = mClickedTrack.get();

   /// Someone has just clicked the mouse.  What do we do?
   if (!IsSampleEditingPossible(
         event, rect, viewInfo, pTrack, rect.width))
      return Cancelled;

   /// We're in a track view and zoomed enough to see the samples.
   mRect = rect;

   //convert t0 to samples
   mClickedStartPixel = viewInfo.TimeToPosition(t0);

   //Determine how drawing should occur.  If alt is down,
   //do a smoothing, instead of redrawing.
   if (event.m_altDown)
   {
      mAltKey = true;
      //*************************************************
      //***  ALT-DOWN-CLICK (SAMPLE SMOOTHING)        ***
      //*************************************************
      //
      //  Smoothing works like this:  There is a smoothing kernel radius constant that
      //  determines how wide the averaging window is.  Plus, there is a smoothing brush radius,
      //  which determines how many pixels wide around the selected pixel this smoothing is applied.
      //
      //  Samples will be replaced by a mixture of the original points and the smoothed points,
      //  with a triangular mixing probability whose value at the center point is
      //  SMOOTHING_PROPORTION_MAX and at the far bounds is SMOOTHING_PROPORTION_MIN

      //Get the region of samples around the selected point
      size_t sampleRegionSize = 1 + 2 * (SMOOTHING_KERNEL_RADIUS + SMOOTHING_BRUSH_RADIUS);
      Floats sampleRegion{ sampleRegionSize };
      Floats newSampleRegion{ 1 + 2 * (size_t)SMOOTHING_BRUSH_RADIUS };

      //Get a sample from the clip to do some tricks on.
      constexpr auto iChannel = 0u;
      constexpr auto mayThrow = false;
      const auto sampleRegionRange = mClickedTrack->GetFloatsCenteredAroundTime(
         t0, iChannel, sampleRegion.get(),
         SMOOTHING_KERNEL_RADIUS + SMOOTHING_BRUSH_RADIUS, mayThrow);

      //Go through each point of the smoothing brush and apply a smoothing operation.
      for (auto jj = -SMOOTHING_BRUSH_RADIUS; jj <= SMOOTHING_BRUSH_RADIUS; ++jj) {
         float sumOfSamples = 0;
         for (auto ii = -SMOOTHING_KERNEL_RADIUS; ii <= SMOOTHING_KERNEL_RADIUS; ++ii) {
            //Go through each point of the smoothing kernel and find the average
            const auto sampleRegionIndex =
               ii + jj + SMOOTHING_KERNEL_RADIUS + SMOOTHING_BRUSH_RADIUS;
            const auto inRange = sampleRegionRange.first <= sampleRegionIndex &&
                                 sampleRegionIndex < sampleRegionRange.second;
            if (!inRange)
               continue;
            //The average is a weighted average, scaled by a weighting kernel that is simply triangular
            // A triangular kernel across N items, with a radius of R ( 2 R + 1 points), if the farthest:
            // points have a probability of a, the entire triangle has total probability of (R + 1)^2.
            //      For sample number ii and middle brush sample M,  (R + 1 - abs(M-ii))/ ((R+1)^2) gives a
            //   legal distribution whose total probability is 1.
            //
            //
            //                weighting factor                       value
            sumOfSamples += (SMOOTHING_KERNEL_RADIUS + 1 - abs(ii)) *
                            sampleRegion[sampleRegionIndex];
         }
         newSampleRegion[jj + SMOOTHING_BRUSH_RADIUS] =
            sumOfSamples /
               ((SMOOTHING_KERNEL_RADIUS + 1) *(SMOOTHING_KERNEL_RADIUS + 1));
      }


      // Now that the NEW sample levels are determined, go through each and mix it appropriately
      // with the original point, according to a 2-part linear function whose center has probability
      // SMOOTHING_PROPORTION_MAX and extends out SMOOTHING_BRUSH_RADIUS, at which the probability is
      // SMOOTHING_PROPORTION_MIN.  _MIN and _MAX specify how much of the smoothed curve make it through.

      float prob;

      for (auto jj = -SMOOTHING_BRUSH_RADIUS; jj <= SMOOTHING_BRUSH_RADIUS; ++jj) {

         prob =
            SMOOTHING_PROPORTION_MAX -
            (float)abs(jj) / SMOOTHING_BRUSH_RADIUS *
               (SMOOTHING_PROPORTION_MAX - SMOOTHING_PROPORTION_MIN);

         newSampleRegion[jj + SMOOTHING_BRUSH_RADIUS] =
            newSampleRegion[jj + SMOOTHING_BRUSH_RADIUS] * prob +
            sampleRegion[SMOOTHING_BRUSH_RADIUS + SMOOTHING_KERNEL_RADIUS + jj] *
               (1 - prob);
      }
      // Set a range of samples around the mouse event
      // Don't require dithering later
      mClickedTrack->SetFloatsCenteredAroundTime(
         t0, iChannel, newSampleRegion.get(), SMOOTHING_BRUSH_RADIUS,
         narrowestSampleFormat);

      // mLastDragSampleValue will not be used
   }
   else
   {
      mAltKey = false;
      //*************************************************
      //***   PLAIN DOWN-CLICK (NORMAL DRAWING)       ***
      //*************************************************

      //Otherwise (e.g., the alt button is not down) do normal redrawing, based on the mouse position.
      const float newLevel = FindSampleEditingLevel(event, viewInfo, t0);

      //Set the sample to the point of the mouse event
      // Don't require dithering later

      constexpr auto iChannel = 0u;
      mClickedTrack->SetFloatAtTime(
         t0, iChannel, newLevel, narrowestSampleFormat);

      mLastDragSampleValue = newLevel;
   }

   //Set the member data structures for drawing
   mLastDragPixel = mClickedStartPixel;

   // Sample data changed on either branch, so refresh the track display.
   return RefreshCell;
}

UIHandle::Result SampleHandle::Drag
(const TrackPanelMouseEvent &evt, AudacityProject *pProject)
{
   using namespace RefreshCode;
   const wxMouseEvent &event = evt.event;
   const auto &viewInfo = ViewInfo::Get( *pProject );

   const bool unsafe = ProjectAudioIO::Get( *pProject ).IsAudioActive();

   /// Someone has just clicked the mouse.  What do we do?
   const auto samplesVisible = IsSampleEditingPossible(
      event, mRect, viewInfo, mClickedTrack.get(), mRect.width);

   if (unsafe || !samplesVisible)
   {
      this->Cancel(pProject);
      return RefreshCell | Cancelled;
   }

   //*************************************************
   //***    DRAG-DRAWING                           ***
   //*************************************************

   //No dragging effects if the alt key is down--
   //Don't allow left-right dragging for smoothing operation
   if (mAltKey)
      return RefreshNone;

   const double t0 = viewInfo.PositionToTime(mLastDragPixel);
   const double t1 = viewInfo.PositionToTime(event.m_x, mRect.x);

   const int x1 =
      event.m_controlDown ? mClickedStartPixel : viewInfo.TimeToPosition(t1);
   const float newLevel = FindSampleEditingLevel(event, viewInfo, t0);
   const auto start = std::min(t0, t1);
   const auto end = std::max(t0, t1);
   // For fast pencil movements covering more than one sample between two
   // updates, we draw a line going from v0 at t0 to v1 at t1.
   const auto interpolator = [t0, t1, v0 = mLastDragSampleValue,
                              v1 = newLevel](double t) {
      if (t0 == t1)
         return v1;
      const auto gradient = (v1 - v0) / (t1 - t0);
      const auto value = static_cast<float>(gradient * (t - t0) + v0);
      // t may be outside t0 and t1, and we don't want to extrapolate.
      return std::clamp(value, std::min(v0, v1), std::max(v0, v1));
   };
   constexpr auto iChannel = 0u;
   mClickedTrack->SetFloatsWithinTimeRange(
      start, end, iChannel, interpolator, narrowestSampleFormat);

   mLastDragPixel = x1;
   mLastDragSampleValue = newLevel;

   return RefreshCell;
}

HitTestPreview SampleHandle::Preview
(const TrackPanelMouseState &st, AudacityProject *pProject)
{
   const bool unsafe = ProjectAudioIO::Get( *pProject ).IsAudioActive();
   return HitPreview(st.state, pProject, unsafe);
}

UIHandle::Result SampleHandle::Release
(const TrackPanelMouseEvent &, AudacityProject *pProject,
 wxWindow *)
{
   const bool unsafe = ProjectAudioIO::Get( *pProject ).IsAudioActive();
   if (unsafe)
      return this->Cancel(pProject);

   //*************************************************
   //***    UP-CLICK  (Finish drawing)             ***
   //*************************************************
   //On up-click, send the state to the undo stack
   mClickedTrack.reset();       //Set this to NULL so it will catch improper drag events.
   ProjectHistory::Get( *pProject ).PushState(XO("Moved Samples"),
      XO("Sample Edit"),
      UndoPush::CONSOLIDATE);

   // No change to draw since last drag
   return RefreshCode::RefreshNone;
}

UIHandle::Result SampleHandle::Cancel(AudacityProject *pProject)
{
   mClickedTrack.reset();
   ProjectHistory::Get( *pProject ).RollbackState();
   return RefreshCode::RefreshCell;
}

float SampleHandle::FindSampleEditingLevel
   (const wxMouseEvent &event, const ViewInfo &viewInfo, double t0)
{
   // Calculate where the mouse is located vertically (between +/- 1)
   float zoomMin, zoomMax;
   auto &cache = WaveformScale::Get(*mClickedTrack);
   cache.GetDisplayBounds(zoomMin, zoomMax);

   const int yy = event.m_y - mRect.y;
   const int height = mRect.GetHeight();
   auto &settings = WaveformSettings::Get(*mClickedTrack);
   const bool dB = !settings.isLinear();
   float newLevel =
      ::ValueOfPixel(yy, height, false, dB,
         settings.dBRange, zoomMin, zoomMax);

   //Take the envelope into account
   const auto time = viewInfo.PositionToTime(event.m_x, mRect.x);
   Envelope *const env = mClickedTrack->GetEnvelopeAtTime(time);
   if (env)
   {
      // Calculate sample as it would be rendered
      double envValue = env->GetValue(t0);
      if (envValue > 0)
         newLevel /= envValue;
      else
         newLevel = 0;

      //Make sure the NEW level is between +/-1
      newLevel = std::max(-1.0f, std::min(1.0f, newLevel));
   }

   return newLevel;
}
