/*****************************************************************************
* Copyright 2015-2016 Alexander Barthel albar965@mailbox.org
*
* This program is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program.  If not, see <http://www.gnu.org/licenses/>.
*****************************************************************************/

#include "profilewidget.h"
#include <algorithm>

#include <gui/mainwindow.h>
#include "geo/calculations.h"
#include "common/mapcolors.h"
#include "ui_mainwindow.h"
#include <QPainter>
#include <QLocale>
#include <QTimer>
#include <QGuiApplication>
#include <QElapsedTimer>
#include <QFutureWatcher>
#include <QMouseEvent>
#include <QRubberBand>
#include <QtConcurrent/QtConcurrentRun>
#include <mapgui/symbolpainter.h>
#include "mapgui/mapwidget.h"
#include <route/routecontroller.h>

#include <marble/ElevationModel.h>
#include <marble/GeoDataCoordinates.h>

const int UPDATE_TIMEOUT = 1000;
const int X0 = 65, Y0 = 14;

using Marble::GeoDataCoordinates;
using atools::geo::Pos;

ProfileWidget::ProfileWidget(MainWindow *parent)
  : QWidget(parent), parentWindow(parent)
{
  setSizePolicy(QSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding));
  elevationModel = parentWindow->getElevationModel();
  routeController = parentWindow->getRouteController();

  updateTimer = new QTimer(this);
  updateTimer->setSingleShot(true);
  connect(updateTimer, &QTimer::timeout, this, &ProfileWidget::updateTimeout);

  connect(elevationModel, &Marble::ElevationModel::updateAvailable,
          this, &ProfileWidget::updateElevation);
  connect(&watcher, &QFutureWatcher<ElevationLegList>::finished, this, &ProfileWidget::updateFinished);

  setMouseTracking(true);
}

ProfileWidget::~ProfileWidget()
{
  if(future.isRunning() || future.isStarted())
  {
    terminate = true;
    future.waitForFinished();
  }
}

void ProfileWidget::routeChanged(bool geometryChanged)
{
  if(!visible)
    return;

  if(geometryChanged)
  {
    qDebug() << "Profile route geometry changed";
    updateTimer->start(UPDATE_TIMEOUT);
  }
  else
  {
    updateScreenCoords();
    update();
  }
}

void ProfileWidget::simDataChanged(const atools::fs::sc::SimConnectData& simulatorData)
{
  if(parentWindow->getMapWidget()->getShownMapFeatures() & maptypes::AIRCRAFT &&
     !routeController->isFlightplanEmpty())
  {
    simData = simulatorData;

    int index = routeController->nearestLegIndex(simData.getPosition());
    if(index != -1)
    {
      if(index >= routeController->getRouteMapObjects().size())
        index = routeController->getRouteMapObjects().size() - 1;
      aircraftDistanceFromStart = 0.f;
      for(int i = 0; i <= index; i++)
      {
        const RouteMapObject& nearestRmo = routeController->getRouteMapObjects().at(i);
        aircraftDistanceFromStart += nearestRmo.getDistanceTo();
      }
      const atools::geo::Pos& position = routeController->getRouteMapObjects().at(index).getPosition();
      aircraftDistanceFromStart -= atools::geo::meterToNm(position.distanceMeterTo(simData.getPosition()));

      if(simData.getPosition().getAltitude() > maxHeight)
        updateScreenCoords();
      update();
    }
  }
  else
  {
    bool valid = simData.getPosition().isValid();
    simData = atools::fs::sc::SimConnectData();
    if(valid)
      update();
  }
}

void ProfileWidget::disconnectedFromSimulator()
{
  simData = atools::fs::sc::SimConnectData();
  updateScreenCoords();
  update();
}

void ProfileWidget::updateScreenCoords()
{
  int w = rect().width() - X0 * 2, h = rect().height() - Y0;

  // Add 1000 ft buffer and round up to the next 500 feet
  maxRouteElevationFt = std::ceil((legList.maxRouteElevation + 1000.f) / 500.f) * 500.f;
  flightplanAltFt = static_cast<float>(routeController->getFlightplan()->getCruisingAlt());
  maxHeight = std::max(maxRouteElevationFt, flightplanAltFt);

  if(simData.getPosition().isValid() &&
     parentWindow->getMapWidget()->getShownMapFeatures() & maptypes::AIRCRAFT &&
     !routeController->isFlightplanEmpty())
    maxHeight = std::max(maxHeight, simData.getPosition().getAltitude());

  vertScale = h / maxHeight;
  horizScale = w / legList.totalDistance;

  waypointX.clear();
  poly.clear();
  poly.append(QPoint(X0, h + Y0));

  for(const ElevationLeg& leg : legList.elevationLegs)
  {
    waypointX.append(X0 + static_cast<int>(leg.distances.first() * horizScale));

    QPoint lastPt;
    for(int i = 0; i < leg.elevation.size(); i++)
    {
      float alt = leg.elevation.at(i).getAltitude();
      QPoint pt(X0 + static_cast<int>(leg.distances.at(i) * horizScale),
                Y0 + static_cast<int>(h - alt * vertScale));

      if(lastPt.isNull() || i == leg.elevation.size() - 1 || (lastPt - pt).manhattanLength() > 2)
      {
        poly.append(pt);
        lastPt = pt;
      }
    }
  }
  waypointX.append(X0 + w);
  poly.append(QPoint(X0 + w, h + Y0));
}

void ProfileWidget::paintEvent(QPaintEvent *)
{
  QElapsedTimer etimer;
  etimer.start();

  int w = rect().width() - X0 * 2, h = rect().height() - Y0;

  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing);
  painter.fillRect(rect(), QBrush(Qt::white));
  painter.fillRect(X0, 0, w, h + Y0, QBrush(QColor::fromRgb(204, 204, 255)));

  SymbolPainter symPainter;

  if(!visible || legList.elevationLegs.isEmpty() || legList.routeMapObjects.isEmpty())
  {
    symPainter.textBox(&painter, {"No Route loaded."}, QPen(Qt::black),
                       X0 + w / 4, Y0 + h / 2, textatt::BOLD, 255);

    return;
  }

  // Draw grey vertical lines for waypoints
  int flightplanY = Y0 + static_cast<int>(h - flightplanAltFt * vertScale);
  painter.setPen(QPen(Qt::lightGray, 2, Qt::SolidLine));
  for(int wpx : waypointX)
    painter.drawLine(wpx, flightplanY, wpx, Y0 + h);

  // Draw the mountains
  painter.setBrush(QColor(Qt::darkGreen));
  painter.setPen(Qt::black);
  painter.drawPolygon(poly);

  // Draw the red maximum elevation line
  painter.setBrush(QColor(Qt::black));
  painter.setPen(QPen(Qt::red, 4, Qt::SolidLine));
  int maxAltY = Y0 + static_cast<int>(h - maxRouteElevationFt * vertScale);
  painter.drawLine(X0, maxAltY, X0 + static_cast<int>(w), maxAltY);

  // Draw the flightplan line
  painter.setPen(QPen(Qt::black, 6, Qt::SolidLine));
  painter.setBrush(QColor(Qt::black));
  painter.drawLine(X0, flightplanY, X0 + static_cast<int>(w), flightplanY);

  painter.setPen(QPen(Qt::yellow, 2, Qt::SolidLine));
  painter.drawLine(X0, flightplanY, X0 + w, flightplanY);

  // Draw flightplan symbols
  // Set default font to bold and reduce size
  QFont font = painter.font();
  float defaultFontSize = font.pointSizeF();
  font.setBold(true);
  font.setPointSizeF(defaultFontSize * 0.8f);
  painter.setFont(font);

  painter.setBackgroundMode(Qt::TransparentMode);

  textflags::TextFlags flags = textflags::IDENT | textflags::ROUTE_TEXT | textflags::ABS_POS;

  for(int i = legList.routeMapObjects.size() - 1; i >= 0; i--)
  {
    const RouteMapObject& rmo = legList.routeMapObjects.at(i);
    int symx = waypointX.at(i);

    switch(rmo.getMapObjectType())
    {
      case maptypes::WAYPOINT:
        symPainter.drawWaypointSymbol(&painter, rmo.getWaypoint(),
                                      QColor(), symx, flightplanY, 8, true, false);
        symPainter.drawWaypointText(&painter, rmo.getWaypoint(), symx - 5, flightplanY + 18,
                                    flags, 10, true, false);
        break;
      case maptypes::USER:
        symPainter.drawUserpointSymbol(&painter, symx, flightplanY, 8, true, false);
        symPainter.textBox(&painter, {rmo.getIdent()}, mapcolors::routeUserPointColor,
                           symx - 5, flightplanY + 18, textatt::BOLD | textatt::ROUTE_BG_COLOR, 255);
        break;
      case maptypes::INVALID:
        symPainter.drawWaypointSymbol(&painter, rmo.getWaypoint(), mapcolors::routeInvalidPointColor,
                                      symx, flightplanY, 8, true, false);
        symPainter.textBox(&painter, {rmo.getIdent()}, mapcolors::routeInvalidPointColor,
                           symx - 5, flightplanY + 18, textatt::BOLD | textatt::ROUTE_BG_COLOR, 255);
        break;
    }
  }

  for(int i = legList.routeMapObjects.size() - 1; i >= 0; i--)
  {
    const RouteMapObject& rmo = legList.routeMapObjects.at(i);
    int symx = waypointX.at(i);

    switch(rmo.getMapObjectType())
    {
      case maptypes::NDB:
        symPainter.drawNdbSymbol(&painter, rmo.getNdb(), symx, flightplanY, 12, true, false);
        symPainter.drawNdbText(&painter, rmo.getNdb(), symx - 5, flightplanY + 18,
                               flags, 10, true, false);
        break;
      case maptypes::VOR:
        symPainter.drawVorSymbol(&painter, rmo.getVor(), symx, flightplanY, 12, true, false, false);
        symPainter.drawVorText(&painter, rmo.getVor(), symx - 5, flightplanY + 18,
                               flags, 10, true, false);
        break;
    }
  }

  font.setBold(true);
  font.setPointSizeF(defaultFontSize);
  painter.setFont(font);
  for(int i = legList.routeMapObjects.size() - 1; i >= 0; i--)
  {
    const RouteMapObject& rmo = legList.routeMapObjects.at(i);
    int symx = waypointX.at(i);

    switch(rmo.getMapObjectType())
    {
      case maptypes::AIRPORT:
        symPainter.drawAirportSymbol(&painter, rmo.getAirport(), symx, flightplanY, 10, false, false);
        symPainter.drawAirportText(&painter, rmo.getAirport(), symx - 5, flightplanY + 22,
                                   flags, 10, false, true, false);
        break;
    }
  }

  // Draw text lables
  float startAlt = legList.routeMapObjects.first().getPosition().getAltitude();
  QString startAltStr = QLocale().toString(startAlt, 'f', 0) + " ft";
  symPainter.textBox(&painter, {startAltStr},
                     QPen(Qt::black), X0 - 8,
                     Y0 + static_cast<int>(h - startAlt * vertScale),
                     textatt::BOLD | textatt::RIGHT, 255);

  float destAlt = legList.routeMapObjects.last().getPosition().getAltitude();
  QString destAltStr = QLocale().toString(destAlt, 'f', 0) + " ft";
  symPainter.textBox(&painter, {destAltStr},
                     QPen(Qt::black), X0 + w + 4,
                     Y0 + static_cast<int>(h - destAlt * vertScale),
                     textatt::BOLD | textatt::LEFT, 255);

  QString maxAlt =
    QLocale().toString(maxRouteElevationFt, 'f', 0) + " ft";
  symPainter.textBox(&painter, {maxAlt},
                     QPen(Qt::red), X0 - 8, maxAltY + 5, textatt::BOLD | textatt::RIGHT, 255);

  QString routeAlt = QLocale().toString(routeController->getFlightplan()->getCruisingAlt()) + " ft";
  symPainter.textBox(&painter, {routeAlt},
                     QPen(Qt::black), X0 - 8, flightplanY + 5, textatt::BOLD | textatt::RIGHT, 255);

  // Draw user aircraft
  if(simData.getPosition().isValid() &&
     parentWindow->getMapWidget()->getShownMapFeatures() & maptypes::AIRCRAFT &&
     !routeController->isFlightplanEmpty())
  {
    int acx = X0 + static_cast<int>(aircraftDistanceFromStart * horizScale);
    int acy = Y0 + static_cast<int>(h - simData.getPosition().getAltitude() * vertScale);
    painter.translate(acx, acy);
    painter.rotate(90);
    symPainter.drawAircraftSymbol(&painter, 0, 0, 20);
    painter.resetTransform();

    font.setPointSizeF(defaultFontSize);
    painter.setFont(font);

    QStringList texts;
    texts.append(QString::number(simData.getPosition().getAltitude(), 'f', 0) + " ft");
    texts.append(QString::number(aircraftDistanceFromStart, 'f', 0) + " nm");

    symPainter.textBox(&painter, texts, QPen(Qt::black), acx, acy + 20, textatt::BOLD, 255);
  }

  qDebug() << "profile paint" << etimer.elapsed() << "ms";
}

ProfileWidget::ElevationLegList ProfileWidget::fetchRouteElevationsThread()
{
  using atools::geo::meterToNm;
  using atools::geo::meterToFeet;

  ElevationLegList legs;
  legs.totalNumPoints = 0;
  legs.totalDistance = 0.f;
  legs.maxRouteElevation = 0.f;
  legs.elevationLegs.clear();

  // Need a copy to avoid synchronization problems
  legs.routeMapObjects = routeController->getRouteMapObjects();

  for(int i = 1; i < legs.routeMapObjects.size(); i++)
  {
    if(terminate)
      return legs;

    const RouteMapObject& lastRmo = legs.routeMapObjects.at(i - 1);
    const RouteMapObject& rmo = legs.routeMapObjects.at(i);

    QList<GeoDataCoordinates> elev = elevationModel->heightProfile(
      lastRmo.getPosition().getLonX(), lastRmo.getPosition().getLatY(),
      rmo.getPosition().getLonX(), rmo.getPosition().getLatY());

    if(elev.isEmpty())
    {
      // Workaround for invalid geometry data - add void
      elev.append(GeoDataCoordinates(lastRmo.getPosition().getLonX(),
                                     lastRmo.getPosition().getLatY(), 0., GeoDataCoordinates::Degree));
      elev.append(GeoDataCoordinates(rmo.getPosition().getLonX(),
                                     rmo.getPosition().getLatY(), 0., GeoDataCoordinates::Degree));
    }

    ElevationLeg leg;
    Pos lastPos;
    for(int j = 0; j < elev.size(); j++)
    {
      const GeoDataCoordinates& coord = elev.at(j);
      if(terminate)
        return ElevationLegList();

      Pos pos(coord.longitude(GeoDataCoordinates::Degree),
              coord.latitude(GeoDataCoordinates::Degree), meterToFeet(coord.altitude()));

      // Drop points with similar altitude except the first and last one on a segment
      if(lastPos.isValid() && j != 0 && j != elev.size() - 1 && legs.elevationLegs.size() > 2 &&
         atools::geo::almostEqual(pos.getAltitude(), lastPos.getAltitude(), 10.f))
        continue;

      float alt = pos.getAltitude();
      if(alt > leg.maxElevation)
        leg.maxElevation = alt;
      if(alt > legs.maxRouteElevation)
        legs.maxRouteElevation = alt;

      leg.elevation.append(pos);
      if(j > 0)
      {
        float dist = meterToNm(lastPos.distanceMeterTo(pos));
        legs.totalDistance += dist;
      }
      leg.distances.append(legs.totalDistance);

      legs.totalNumPoints++;
      lastPos = pos;
    }
    legs.elevationLegs.append(leg);
  }

  qDebug() << "elevation legs" << legs.elevationLegs.size() << "total points" << legs.totalNumPoints
           << "total distance" << legs.totalDistance << "max route elevation" << legs.maxRouteElevation;
  return legs;
}

void ProfileWidget::updateElevation()
{
  if(!visible)
    return;

  qDebug() << "Profile update elevation";
  updateTimer->start(UPDATE_TIMEOUT);
}

void ProfileWidget::updateTimeout()
{
  if(!visible)
    return;

  qDebug() << "Profile update elevation timeout";

  if(future.isRunning() || future.isStarted())
  {
    terminate = true;
    future.waitForFinished();
  }

  terminate = false;

  // Start the computation in background
  future = QtConcurrent::run(this, &ProfileWidget::fetchRouteElevationsThread);
  watcher.setFuture(future);
}

void ProfileWidget::updateFinished()
{
  if(!visible)
    return;

  qDebug() << "Profile update finished";

  if(!terminate)
  {
    legList = future.result();
    updateScreenCoords();
    update();
  }
}

void ProfileWidget::showEvent(QShowEvent *)
{
  visible = true;
  updateTimer->start(0);
}

void ProfileWidget::hideEvent(QHideEvent *)
{
  visible = false;
}

void ProfileWidget::mouseMoveEvent(QMouseEvent *mouseEvent)
{
  if(legList.elevationLegs.isEmpty())
    return;

  if(rubberBand == nullptr)
    rubberBand = new QRubberBand(QRubberBand::Line, this);

  int x = mouseEvent->pos().x();
  x = std::max(x, X0);
  x = std::min(x, rect().width() - X0);

  rubberBand->setGeometry(x - 1, 0, 2, rect().height());
  rubberBand->show();

  // Get index for leg
  int index = 0;
  QVector<int>::iterator it = std::lower_bound(waypointX.begin(), waypointX.end(), x);
  if(it != waypointX.end())
  {
    index = static_cast<int>(std::distance(waypointX.begin(), it)) - 1;
    if(index < 0)
      index = 0;
  }
  const ElevationLeg& leg = legList.elevationLegs.at(index);

  // Get from/to text
  QString from = legList.routeMapObjects.at(index).getIdent();
  QString to = legList.routeMapObjects.at(index + 1).getIdent();

  float distance = (x - X0) / horizScale;
  int indexLowDist = 0;
  QVector<float>::const_iterator lowDistIt = std::lower_bound(leg.distances.begin(),
                                                              leg.distances.end(), distance);
  if(lowDistIt != leg.distances.end())
  {
    indexLowDist = static_cast<int>(std::distance(leg.distances.begin(), lowDistIt));
    if(indexLowDist < 0)
      indexLowDist = 0;
  }
  int indexUpperDist = 0;
  QVector<float>::const_iterator upperDistIt = std::upper_bound(leg.distances.begin(),
                                                                leg.distances.end(), distance);
  if(upperDistIt != leg.distances.end())
  {
    indexUpperDist = static_cast<int>(std::distance(leg.distances.begin(), upperDistIt));
    if(indexUpperDist < 0)
      indexUpperDist = 0;
  }
  float alt1 = leg.elevation.at(indexLowDist).getAltitude();
  float alt2 = leg.elevation.at(indexUpperDist).getAltitude();
  float alt = std::abs(alt1 + alt2) / 2.f;

  // Get Position for highlight
  float legdistpart = distance - leg.distances.first();
  float legdist = leg.distances.last() - leg.distances.first();
  const atools::geo::Pos& pos = leg.elevation.first().interpolate(leg.elevation.last(), legdistpart / legdist);

  float maxElev = std::ceil((leg.maxElevation + 1000.f) / 500.f) * 500.f;

  parentWindow->getUi()->labelElevationInfo->setText(
    "<b>" + from + " −> " + to + "</b>, " +
    QString::number(distance, 'f', distance < 100.f ? 1 : 0) + " nm, " +
    " Ground Altitude " + QString::number(alt, 'f', 0) + " ft, " +
    " Above Ground Altitude " + QString::number(flightplanAltFt - alt, 'f', 0) + " ft, " +
    " Leg Safe Altitude " + QString::number(maxElev, 'f', 0) + " ft");

  mouseEvent->accept();

  emit highlightProfilePoint(pos);
}

void ProfileWidget::leaveEvent(QEvent *)
{
  qDebug() << "leave";

  delete rubberBand;
  rubberBand = nullptr;

  parentWindow->getUi()->labelElevationInfo->setText("No information.");

  emit highlightProfilePoint(atools::geo::EMPTY_POS);
}

void ProfileWidget::resizeEvent(QResizeEvent *)
{
  updateScreenCoords();
}
